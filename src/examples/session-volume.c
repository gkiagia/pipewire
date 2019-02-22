/* PipeWire
 *
 * Copyright © 2019 Collabora Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */


#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <stdbool.h>
#include <stdlib.h>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>

#define NAME "pipewire-media-session"

int main(int argc, char *argv[])
{
	int fd;
	struct sockaddr_un addr;
	int name_size;
	const char *runtime_dir = NULL;
	ssize_t r;
	char buf[4096];

	if (argc < 2) {
		fprintf(stderr, "Usage: %s \"volume|mute <role> <value>\"", argv[0]);
		return -1;
	}

	if ((runtime_dir = getenv("XDG_RUNTIME_DIR")) == NULL) {
		fprintf(stderr, "connect failed: XDG_RUNTIME_DIR not set in the environment");
		return -1;
	}

	fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_LOCAL;
	name_size = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/"NAME, runtime_dir) + 1;

	if (name_size > (int) sizeof(addr.sun_path)) {
		fprintf(stderr, "socket path \"%s/"NAME"\" plus null terminator exceeds 108 bytes",
			runtime_dir);
		close(fd);
		return -1;
	}

	if (connect(fd,(const struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("connect");
		close(fd);
		return -1;
	}

	while (true) {
		r = write(fd, argv[1], strlen(argv[1]));
		if (r < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			perror("write");
			r = 0;
			break;
		}
		break;
	}
	if (r == 0) {
		fprintf(stderr, "Nothing was written");
		close(fd);
		return -1;
	}

	while (true) {
		r = read(fd, buf, sizeof(buf));
		if (r < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			perror("read");
			r = 0;
			break;
		}
		break;
	}
	if (r == 0) {
		fprintf(stderr, "EOF");
		close(fd);
		return -1;
	}

	buf[r] = '\0';
	fprintf(stdout, NAME" replied: %s\n", buf);

	close(fd);

	return 0;
}
