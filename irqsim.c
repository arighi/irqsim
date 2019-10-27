/*
 * irqsim: irq-based device simulator
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 *
 * Copyright (C) 2019 Andrea Righi <righi.andrea@gmail.com>
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/circ_buf.h>
#include <linux/kfifo.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>

#define IRQSIM_NR	1
#define FIFO_BUFSIZE	PAGE_SIZE

static int delay = 100;
module_param(delay, int, 0);
MODULE_PARM_DESC(delay, "Time (in ms) to generate a sample");

/* Data produced by the simulated device */
static int irqsim_data;

/* Timer to simulate a periodic IRQ */
static struct timer_list timer;

/* Character device stuff */
static int major;
static struct class *irqsim_class;
static struct cdev irqsim_cdev;

/* Data are stored into a kfifo buffer before passing them to the userspace */
static struct kfifo rx_fifo;

/*
 * NOTE: the usage of kfifo is safe (no need for extra locking), until there is
 * only one concurrent reader and one concurrent writer. Writes are serialized
 * from the interrupt context, readers are serialized using this mutex.
 */
static DEFINE_MUTEX(read_lock);

/* Wait queue to implement blocking I/O from userspace */
static DECLARE_WAIT_QUEUE_HEAD(rx_wait);

/* Generate new data from the simulated device */
static int update_irqsim_data(void)
{
	irqsim_data = max((irqsim_data + 1) % 0x7f, 0x20);
	return irqsim_data;
}

/* Insert a value into the kfifo buffer */
static void produce_data(unsigned char val)
{
	unsigned int len;

	/*
	 * Implement a kind of circular FIFO here (skip oldest element if kfifo
	 * buffer is full).
	 */
	len = kfifo_in(&rx_fifo, &val, sizeof(val));
	if (unlikely(len < sizeof(val)) && printk_ratelimit())
		pr_warning("%s: %zu bytes dropped\n",
				__func__, sizeof(val) - len);
	pr_debug("%s: in %u/%u bytes\n", __func__, len, kfifo_len(&rx_fifo));
}

/* Mutex to serialize kfifo writers within the workqueue handler */
static DEFINE_MUTEX(producer_lock);

/*
 * Mutex to serialize fast_buf consumers: we can use a mutex because consumers
 * run in workqueue handler (kernel thread context).
 */
static DEFINE_MUTEX(consumer_lock);

/*
 * We use an additional "faster" circular buffer to quickly store data from
 * interrupt context, before adding them to the kfifo.
 */
static struct circ_buf fast_buf;

static int fast_buf_get(void)
{
	struct circ_buf *ring = &fast_buf;
	/* prevent the compiler from merging or refetching accesses for tail */
	unsigned long head = READ_ONCE(ring->head);
	unsigned long tail = ring->tail;
	int ret;

	if (unlikely(!CIRC_CNT(head, tail, PAGE_SIZE)))
		return -ENOENT;
	/* read index before reading contents at that index */
	smp_read_barrier_depends();
	/* extract item from the buffer */
	ret = ring->buf[tail];
	/* finish reading descriptor before incrementing tail */
	smp_mb();
	/* increment the tail pointer */
	ring->tail = (tail + 1) & (PAGE_SIZE - 1);

	return ret;
}

static int fast_buf_put(unsigned char val)
{
	struct circ_buf *ring = &fast_buf;
	unsigned long head = ring->head;
	/* prevent the compiler from merging or refetching accesses for tail */
	unsigned long tail = READ_ONCE(ring->tail);

	/* is circular buffer full? */
	if (unlikely(!CIRC_SPACE(head, tail, PAGE_SIZE)))
		return -ENOMEM;
	ring->buf[ring->head] = val;
	/* commit the item before incrementing the head */
	smp_wmb();
	ring->head = (ring->head + 1) & (PAGE_SIZE - 1);

	return 0;
}

/* Clear all data from the circular buffer fast_buf */
static void fast_buf_clear(void)
{
	fast_buf.head = 0;
	fast_buf.tail = 0;
}

/* Workqueue handler: executed by a kernel thread */
static void irqsim_work_func(struct work_struct *w)
{
	int val, cpu;

	/*
         * This code runs from a kernel thread, so softirqs and hard-irqs must
	 * be enabled.
	 */
	WARN_ON_ONCE(in_softirq());
	WARN_ON_ONCE(in_interrupt());

	/*
	 * Pretend to simulate access to per-CPU data, disabling preemption
	 * during the pr_info().
	 */
	cpu = get_cpu();
	pr_info("[CPU#%d] %s\n", cpu, __func__);
	put_cpu();

	while (1) {
		/* Consume data from the circular buffer */
		mutex_lock(&consumer_lock);
		val = fast_buf_get();
		mutex_unlock(&consumer_lock);

		if (val < 0)
			break;

		/* Store data to the kfifo buffer */
		mutex_lock(&producer_lock);
		produce_data(val);
		mutex_unlock(&producer_lock);
	}
	wake_up_interruptible(&rx_wait);
}

/* Workqueue for asynchronous bottom-half processing */
static struct workqueue_struct *irqsim_workqueue;

/*
 * Work item: holds a pointer to the function that is to be executed
 * asynchronously.
 */
static DECLARE_WORK(work, irqsim_work_func);

/*
 * Tasklet handler.
 *
 * NOTE: different tasklets can run concurrently on different processors, but
 * two of the same type of tasklet cannot run simultaneously. Moreover, a
 * tasklet always runs on the same CPU that schedules it.
 */
static void irqsim_tasklet_func(unsigned long __data)
{
	ktime_t tv_start, tv_end;
	s64 nsecs;

	WARN_ON_ONCE(!in_softirq());

	tv_start = ktime_get();
	queue_work(irqsim_workqueue, &work);
	tv_end = ktime_get();

	nsecs = (s64)ktime_to_ns(ktime_sub(tv_end, tv_start));

	pr_info("[CPU#%d] in_softirq: %llu usec",
		smp_processor_id(), (unsigned long long)nsecs >> 10);
}

/* Tasklet for asynchronous bottom-half processing in softirq context */
static DECLARE_TASKLET(irqsim_tasklet, irqsim_tasklet_func, 0);

static void process_data(void)
{
	WARN_ON_ONCE(!irqs_disabled());

	fast_buf_put(update_irqsim_data());
	tasklet_schedule(&irqsim_tasklet);
}

static void timer_handler(struct timer_list *__timer)
{
	ktime_t tv_start, tv_end;
	s64 nsecs;

	/*
	 * We're using a kernel timer to simulate a hard-irq, so we must expect
	 * to be in softirq context here.
	 */
	WARN_ON_ONCE(!in_softirq());

	/*
	 * Disable interrupts for this CPU to simulate a real interrupt
	 * context.
	 */
	local_irq_disable();

	tv_start = ktime_get();
	process_data();
	tv_end = ktime_get();

	nsecs = (s64)ktime_to_ns(ktime_sub(tv_end, tv_start));

	pr_info("[CPU#%d] in_irq: %llu usec",
		smp_processor_id(), (unsigned long long)nsecs >> 10);
	mod_timer(&timer, jiffies + msecs_to_jiffies(delay));

	local_irq_enable();
}

static ssize_t irqsim_read(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	unsigned int read;
	int ret;

	pr_debug("%s(%p, %zd, %lld)\n", __func__, buf, count, *ppos);

	if (unlikely(!access_ok(buf, count)))
		return -EFAULT;
	if (mutex_lock_interruptible(&read_lock))
		return -ERESTARTSYS;
	do {
		ret = kfifo_to_user(&rx_fifo, buf, count, &read);
		if (unlikely(ret < 0))
			break;
		if (read)
			break;
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			break;
		}
		ret = wait_event_interruptible(rx_wait, kfifo_len(&rx_fifo));
	} while (ret == 0);
	pr_debug("%s: out %u/%u bytes\n",
		__func__, read, kfifo_len(&rx_fifo));
	mutex_unlock(&read_lock);

	return ret ? ret : read;
}

static unsigned int irqsim_poll(struct file *file, poll_table *wait)
{
	unsigned int mask = 0;

	pr_debug("%s\n", __func__);
	if (mutex_lock_interruptible(&read_lock))
		return -ERESTARTSYS;
	poll_wait(file, &rx_wait, wait);
	if (kfifo_len(&rx_fifo))
		mask |= POLLIN | POLLRDNORM;
	mutex_unlock(&read_lock);
	pr_debug("%s: mask = %#x\n", __func__, mask);

	return mask;
}

static int irqsim_open(struct inode *inode, struct file *filp)
{
	pr_debug("%s\n", __func__);
	mod_timer(&timer, jiffies + msecs_to_jiffies(delay));
	return 0;
}

static int irqsim_release(struct inode *inode, struct file *filp)
{
	pr_debug("%s\n", __func__);
	del_timer_sync(&timer);
	flush_workqueue(irqsim_workqueue);
	fast_buf_clear();

	return 0;
}

static const struct file_operations irqsim_fops = {
	.read		= irqsim_read,
	.poll		= irqsim_poll,
	.llseek		= no_llseek,
	.open		= irqsim_open,
	.release	= irqsim_release,
	.owner		= THIS_MODULE,
};

static char *irqsim_devnode(struct device *dev, umode_t *mode)
{
	return kasprintf(GFP_KERNEL, "irqsim/%s", dev_name(dev));
}

static int __init irqsim_init(void)
{
	dev_t dev_id;
	int ret;

	if (kfifo_alloc(&rx_fifo, FIFO_BUFSIZE, GFP_KERNEL) < 0)
		return -ENOMEM;

	/* Register major/minor numbers */
	ret = alloc_chrdev_region(&dev_id, 0, IRQSIM_NR, "irqsim");
	if (ret)
		goto error_alloc;
	major = MAJOR(dev_id);

	/* Add the character device to the system */
	cdev_init(&irqsim_cdev, &irqsim_fops);
	ret = cdev_add(&irqsim_cdev, dev_id, IRQSIM_NR);
	if (ret) {
		kobject_put(&irqsim_cdev.kobj);
		goto error_region;
	}

	/* Create a class structure */
	irqsim_class = class_create(THIS_MODULE, "irqsim");
	if (IS_ERR(irqsim_class)) {
		printk(KERN_ERR "error creating irqsim class\n");
		ret = PTR_ERR(irqsim_class);
		goto error_cdev;
	}
	irqsim_class->devnode = irqsim_devnode;

	/* Register the device with sysfs */
	device_create(irqsim_class, NULL, MKDEV(major, 0), NULL, "irqsimctl");

	/* Allocate fast circular buffer */
	fast_buf.buf = vmalloc(PAGE_SIZE);
	if (!fast_buf.buf) {
		device_destroy(irqsim_class, dev_id);
		class_destroy(irqsim_class);
		ret = -ENOMEM;
		goto error_cdev;
	}

	/* Create the workqueue */
	irqsim_workqueue = alloc_workqueue("irqsimd",
				WQ_UNBOUND, WQ_MAX_ACTIVE);
	if (!irqsim_workqueue) {
		vfree(fast_buf.buf);
		device_destroy(irqsim_class, dev_id);
		class_destroy(irqsim_class);
		ret = -ENOMEM;
		goto error_cdev;
	}

	/* Setup the timer */
	timer_setup(&timer, timer_handler, 0);

	pr_info("register new irqsim device: %d,%d\n", major, 0);
out:
	return ret;
error_cdev:
	cdev_del(&irqsim_cdev);
error_region:
	unregister_chrdev_region(dev_id, IRQSIM_NR);
error_alloc:
	kfifo_free(&rx_fifo);
	goto out;
}

static void __exit irqsim_exit(void)
{
	dev_t dev_id = MKDEV(major, 0);

	del_timer_sync(&timer);
	tasklet_kill(&irqsim_tasklet);
	flush_workqueue(irqsim_workqueue);
	destroy_workqueue(irqsim_workqueue);
	vfree(fast_buf.buf);
	device_destroy(irqsim_class, dev_id);
	class_destroy(irqsim_class);
	cdev_del(&irqsim_cdev);
	unregister_chrdev_region(dev_id, IRQSIM_NR);

	kfifo_free(&rx_fifo);
	pr_info("irqsim: unloaded\n");
}

module_init(irqsim_init);
module_exit(irqsim_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("irq-based device simulator");
MODULE_AUTHOR("Andrea Righi <righi.andrea@gmail.com>");
