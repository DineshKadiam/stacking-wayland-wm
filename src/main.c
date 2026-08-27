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

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cow/cmd.h"
#include "cow/command.h"
#include "cow/config.h"
#include "cow/ipc.h"
#include "cow/log.h"
#include "cow/memory.h"
#include "cow/menu.h"
#include "cow/wm.h"

#ifndef COW_VERSION
#define COW_VERSION "unknown"
#endif

static const char *usage =
    "usage: cow [-hsv] [-c config] [-l level] [-o file] "
    "[-C auto|always|never]\n";

int
main(int argc, char *argv[])
{
	const char *config_path = NULL;
	const char *log_path = getenv("COW_LOG_FILE");
	const char *log_level_value = getenv("COW_LOG_LEVEL");
	const char *log_colour_value = getenv("COW_LOG_COLOR");
	enum cow_log_level log_level = COW_LOG_INFO;
	enum cow_log_colour log_colour = COW_LOG_COLOUR_AUTO;
	int	    ch;

	while ((ch = getopt(argc, argv, "C:c:hl:o:sv")) != -1) {
		switch (ch) {
		case 'C':
			log_colour_value = optarg;
			break;
		case 'c':
			config_path = optarg;
			break;
		case 'l':
			log_level_value = optarg;
			break;
		case 'o':
			log_path = optarg;
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
	if (log_level_value != NULL &&
	    !cow_log_level_parse(log_level_value, &log_level)) {
		fprintf(stderr, "cow: invalid log level: %s\n", log_level_value);
		return (EXIT_FAILURE);
	}
	if (log_colour_value != NULL &&
	    !cow_log_colour_parse(log_colour_value, &log_colour)) {
		fprintf(stderr, "cow: invalid log colour mode: %s\n",
		    log_colour_value);
		return (EXIT_FAILURE);
	}
	if (cow_log_init(log_path, log_level, log_colour) < 0) {
		fprintf(stderr, "cow: cannot open log %s: %s\n", log_path,
		    strerror(errno));
		return (EXIT_FAILURE);
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
	 * Initialize lists used while loading config before cow_wm_init()
	 * connects to River.  menu-add needs the menu lists, while a top-level
	 * script may snapshot windows, outputs, or desks; those sources should
	 * produce empty lists during initial startup rather than walking
	 * uninitialized list heads.  cow_wm_init() initializes them again after
	 * preserving the menus created here.
	 */
	wl_list_init(&cow_wm.menus);
	wl_list_init(&cow_wm.open_menus);
	wl_list_init(&cow_wm.menu_graveyard);
	wl_list_init(&cow_wm.views);
	wl_list_init(&cow_wm.outputs);

	/*
	 * Resolve the config path: use the explicit -c path if given,
	 * otherwise search the default locations.  If nothing is found,
	 * proceed with built-in defaults and no warning -- the user may
	 * simply not have a config file yet.
	 */
	if (config_path == NULL)
		config_path = cow_config_default_path();

	if (config_path != NULL && !cow_ipc_load_config(config_path))
		cow_log_warn("config load failed, using defaults");

	/*
	 * Decoration-click defaults.  Each (region, clicks) pair is
	 * installed only if the user didn't declare a binding for it
	 * in the config, so partial overrides leave the other
	 * defaults intact.
	 */
	cow_decoration_register_default_bindings();
	/* Ignore SIGPIPE -- child processes (xterm etc.) may cause this */
	signal(SIGPIPE, SIG_IGN);

	if (!cow_wm_init()) {
		cow_config_destroy(cow_config);
		cow_log_fini();
		return EXIT_FAILURE;
	}

	cow_log_info("connected to river compositor");

	cow_wm_run();

	if (cow_wm_restart_requested) {
		/* Restart: save state and execvp without tearing down River.
		 * cow_wm_restart() calls _exit() on failure, never returns. */
		cow_wm_restart(argv);
	}

	cow_wm_fini();
	cow_config_destroy(cow_config);
	cow_log_fini();
	return EXIT_SUCCESS;
}
