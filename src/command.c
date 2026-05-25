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
	cow_command_execute_with_event(cmd, NULL);
}

void
cow_command_execute_with_event(const char *cmd, const char *event_json)
{
	pid_t outer, inner;
	int   status;

	/*
	 * Double-fork so the grandchild is reparented to init and we don't
	 * leave a zombie.  The outer child exits immediately after forking
	 * the inner one; we reap it below.
	 */
	outer = fork();
	if (outer < 0) {
		fprintf(stderr, "cow_command_execute: fork: %s\n",
		    strerror(errno));
		return;
	}
	if (outer == 0) {
		inner = fork();
		if (inner < 0)
			_exit(127);
		if (inner == 0) {
			setsid();

			/*
			 * If DISPLAY is not set, try to find an active XWayland
			 * display for X11 apps like xterm.  WAYLAND_DISPLAY is
			 * inherited from the parent environment.
			 */
			if (getenv("DISPLAY") == NULL) {
				const char *xdisplay = find_xwayland_display();
				if (xdisplay != NULL)
					setenv("DISPLAY", xdisplay, 0);
			}

			/*
			 * Per-event payload: expose the rule subsystem's
			 * JSON payload as $COW_EVENT for the spawned
			 * shell.  setenv happens in the grandchild only,
			 * so no other process (cow itself or any
			 * concurrent rule fire) ever sees this variable.
			 */
			if (event_json != NULL && event_json[0] != '\0')
				setenv("COW_EVENT", event_json, 1);

			execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
			_exit(127); /* exec failed */
		}
		_exit(0);
	}

	/* Reap the outer child; the grandchild is reparented to init. */
	for (;;) {
		if (waitpid(outer, &status, 0) < 0) {
			if (errno == EINTR)
				continue;
		}
		return;
	}
}
