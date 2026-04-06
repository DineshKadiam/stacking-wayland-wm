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
/*
 * cow - A window manager for the river Wayland compositor.
 *
 * cow implements cwm-inspired window management with FVWM/MWM-style
 * server-side decorations and sheets (virtual desktops).
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cow/cmd.h"
#include "cow/command.h"
#include "cow/config.h"
#include "cow/ipc.h"
#include "cow/memory.h"
#include "cow/wm.h"

#ifndef COW_VERSION
#define COW_VERSION "unknown"
#endif

static const char *usage = "usage: cow [-hv] [-c config]\n";

int
main(int argc, char *argv[])
{
	const char *config_path = NULL;
	int	    ch;

	while ((ch = getopt(argc, argv, "c:hv")) != -1) {
		switch (ch) {
		case 'c':
			config_path = optarg;
			break;
		case 'v':
			printf("cow %s\n", COW_VERSION);
			return EXIT_SUCCESS;
		case 'h':
		default:
			fputs(usage, ch == 'h' ? stdout : stderr);
			return ch == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	/*
	 * Ensure XDG_CURRENT_DESKTOP and XDG_SESSION_TYPE are set so that
	 * xdg-desktop-portal can select the correct backend (xdg-desktop-
	 * portal-wlr) for screen sharing.  When cow is launched via a display
	 * manager the .desktop file's DesktopNames= field already sets
	 * XDG_CURRENT_DESKTOP; the setenv calls below are a fallback for
	 * direct invocation.  The 0 argument means we never overwrite a value
	 * already present in the environment.
	 */
	setenv("XDG_CURRENT_DESKTOP", "river", 0);
	setenv("XDG_SESSION_TYPE", "wayland", 0);

	cow_config = cow_config_create();

	/*
	 * Resolve the config path: use the explicit -c path if given,
	 * otherwise search the default locations.  If nothing is found,
	 * proceed with built-in defaults and no warning -- the user may
	 * simply not have a config file yet.
	 */
	if (config_path == NULL)
		config_path = cow_config_default_path();

	if (config_path != NULL && !cow_ipc_load_config(config_path))
		fprintf(stderr,
		    "cow: warning: config load failed, using defaults\n");
	/* Ignore SIGPIPE -- child processes (xterm etc.) may cause this */
	signal(SIGPIPE, SIG_IGN);

	/* Handle SIGTERM/SIGINT for clean shutdown (saves state) */
	signal(SIGTERM, cow_wm_signal_handler);
	signal(SIGINT, cow_wm_signal_handler);

	/* Handle SIGUSR1 for restart */
	signal(SIGUSR1, cow_wm_restart_handler);

	if (!cow_wm_init()) {
		cow_config_destroy(cow_config);
		return EXIT_FAILURE;
	}

	fprintf(stderr, "cow: connected to river compositor\n");

	cow_wm_run();

	if (cow_wm_restart_requested) {
		/* Restart: save state and execvp without tearing down River.
		 * cow_wm_restart() calls _exit() on failure, never returns. */
		cow_wm_restart(argv);
	}

	cow_wm_fini();
	cow_config_destroy(cow_config);
	return EXIT_SUCCESS;
}
