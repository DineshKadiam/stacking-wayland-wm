/*
 * Copyright (c) 2026 Thomas Adam
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cow/compat.h"
#include "cow/command.h"

/*
 * Find the DISPLAY for XWayland if one is running.
 * Checks /tmp/.X11-unix/ for the newest socket, then verifies
 * a corresponding Xwayland process exists.
 * Returns a static string like ":0" or NULL.
 */
static const char *
find_xwayland_display(void)
{
	static char display_buf[16];
	DIR	   *dir = opendir("/tmp/.X11-unix");
	if (dir == NULL)
		return NULL;

	struct dirent *ent;
	time_t	       newest_mtime = 0;
	int	       newest_num   = -1;

	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] != 'X')
			continue;
		const char *errstr;
		int num = (int)strtonum(ent->d_name + 1, 0, 64, &errstr);
		if (errstr != NULL)
			continue;

		char path[sizeof("/tmp/.X11-unix/") + NAME_MAX];
		snprintf(path, sizeof(path), "/tmp/.X11-unix/%s", ent->d_name);

		struct stat st;
		if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode)) {
			if (st.st_mtime > newest_mtime) {
				newest_mtime = st.st_mtime;
				newest_num   = num;
			}
		}
	}
	closedir(dir);

	if (newest_num < 0)
		return NULL;

	snprintf(display_buf, sizeof(display_buf), ":%d", newest_num);
	return display_buf;
}

void
cow_command_execute(const char *cmd)
{
	pid_t child;
	int   status;

	child = fork();
	if (child == 0) {
		child = fork();
		if (child == 0) {
			setsid();

			/*
			 * Ensure WAYLAND_DISPLAY is set for Wayland-native
			 * apps. It should already be inherited, but be
			 * explicit.
			 */

			/*
			 * If DISPLAY is not set, try to find an active XWayland
			 * display for X11 apps like xterm.
			 */
			if (getenv("DISPLAY") == NULL) {
				const char *xdisplay = find_xwayland_display();
				if (xdisplay != NULL)
					setenv("DISPLAY", xdisplay, 0);
			}

			execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
		}
		_exit(child == -1);
	}

	for (;;) {
		waitpid(child, &status, 0);
		if (errno == EINTR) {
			continue;
		} else {
			return;
		}
	}
}
