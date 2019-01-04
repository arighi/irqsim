/*
 *  pollread - poll() + read() testcase
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  Copyright (C) 2010 Andrea Righi <arighi@develer.com>
 */

#define _GNU_SOURCE
#define __USE_GNU

#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <sched.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>

/* Read timeout (in ms) */
#define TIMEOUT		10

#ifdef DEBUG
#define pr_debug(format, args...) fprintf(stderr, "debug: " format, ##args)
#else
#define pr_debug(format, args...)
#endif

ssize_t read_timeout(int fd, void *buf, size_t count, size_t timeout)
{
	struct pollfd pfd = {};
	ssize_t len = 0;

	pr_debug("entering %s(%d, %p, %zi, %zi)\n",
			__func__, fd, buf, count, timeout);
	pfd.fd = fd;
	pfd.events = POLLIN;

	while (len <= 0) {
		/* poll */
		pr_debug("poll\n");
		len = poll(&pfd, 1, timeout);
		pr_debug("poll: %zd\n", len);
		if (len < 0)
			break;
		else if (!len) {
			len = -ETIMEDOUT;
			break;
		}
		/* read */
		pr_debug("read\n");
		len = read(fd, buf, count);
		pr_debug("read: %zd\n", len);
		if (!len || (len < 0))
			sched_yield();
	}
	pr_debug("exiting %s(%d, %p, %zi, %zi) = %zi\n",
			__func__, fd, buf, count, timeout, len);
	return len;
}

int main(int argc, char **argv)
{
	int i;

	fcntl(fileno(stdin), F_SETFL, O_NONBLOCK);

	setbuf(stdin, NULL);
	setbuf(stdout, NULL);

	for (i = 0; ; i++) {
		char c;

		if (read_timeout(fileno(stdin), &c, sizeof(c), TIMEOUT) > 0)
			fputc(c, stdout);
	}
	return 0;
}
