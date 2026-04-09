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
#if !defined(COW_CONFIGURATION_NEW_H)
#define COW_CONFIGURATION_NEW_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

#include "cow/colour.h"
#include "cow/vector.h"

struct cow_output;
struct cow_desk;

/*
 * Border style enum (carried from original cow).
 */
enum cow_border_style {
  COW_BORDER_STYLE_NONE = -1,
  COW_BORDER_STYLE_MWM = 0,
  COW_BORDER_STYLE_FVWM = 1,
};

/*
 * Window placement mode.
 */
enum cow_placement_mode {
  COW_PLACEMENT_CASCADE,
  COW_PLACEMENT_UNDERMOUSE,
  COW_PLACEMENT_CENTRE,
};

/*
 * Per-edge border colors.
 */
struct cow_border_colour {
  float n[4], s[4], e[4], w[4];
  float nw[4], ne[4], sw[4], se[4];
  bool has_corners;
  bool has_nw, has_ne, has_sw, has_se;
};

/*
 * A single parsed keybinding action string from the config.
 */
struct cow_binding_entry {
  struct wl_list link;
  char *key_string;    /* e.g. "L+Return", "LS+q" */
  char *action_string; /* e.g. "action-terminal", "quit", "view-quit" */
};

/*
 * A single parsed mouse binding from the config.
 */
struct cow_mouse_binding_entry {
  struct wl_list link;
  char *key_string;    /* e.g. "L+left" */
  char *action_string; /* e.g. "mode-enter-move" */
};

/*
 * A named action (user-defined command).
 */
struct cow_action_entry {
  struct wl_list link;
  char *name;
  char *command;
};

/*
 * Button action enum.
 */
enum cow_button_action {
  COW_BUTTON_ACTION_CLOSE,
  COW_BUTTON_ACTION_MAXIMIZE,
  COW_BUTTON_ACTION_ICONIFY,
  COW_BUTTON_ACTION_NONE,
};

/*
 * Titlebar button config.
 */
struct cow_titlebar_button_config {
  struct cow_vector_button vector;
  enum cow_button_action action;
};

#define COW_TITLEBAR_NUM_BUTTONS 3

/*
 * Container tab strip styling.
 * Controls the colours used for tab labels inside a container titlebar.
 * Defaults are set in cow_config_create(); all four values can be
 * overridden via 'set container.tab.*' commands.
 */
struct cow_container_config {
  float tab_active[4];      /* background of the active tab label   */
  float tab_inactive[4];    /* background of inactive tab labels    */
  float tab_fg_active[4];   /* foreground (text) of the active tab  */
  float tab_fg_inactive[4]; /* foreground (text) of inactive tabs   */

  /* Set to true when the user explicitly configures the value.
   * When false, titlebar.c falls back to the titlebar's base/dk/fg. */
  bool has_tab_active;
  bool has_tab_inactive;
  bool has_tab_fg_active;
  bool has_tab_fg_inactive;

  /* Whether inactive tabs are drawn with a sunken bevel.
   * Default false (flat); set true for the classic depressed look. */
  bool tab_sunken_inactive;
};

/*
 * Titlebar configuration.
 */
struct cow_titlebar_config {
  bool enabled;
  struct cow_titlebar_button_config buttons[COW_TITLEBAR_NUM_BUTTONS];
  float active_colour[4];
  float inactive_colour[4];
  float fg_active[4];
  float fg_inactive[4];
  char *font_name;
  /* Explicit height override; 0 means derive from font. */
  int height;
};

/*
 * Per-app-id view configuration.
 */
struct cow_view_config_entry {
  struct wl_list link;
  char *app_id;       /* match pattern (app_id or title) */
  char *title;        /* title match pattern (if set) */
  bool use_glob;      /* true = fnmatch glob, false = exact */
  int assign_desk_nr; /* desk to assign view to; -1 = default */
  bool sticky;
  bool focus;

  /* Constraints: only apply to views on this output/desk.
   * NULL = match any. */
  char *output_name;
  int desk_nr; /* -1 = any */

  /* Visual overrides (has_* flags indicate which are set) */
  bool has_border;
  int border;

  /* Per-edge border colors (full cow_border_colour override) */
  bool has_border_colour_active;
  struct cow_border_colour border_colour_active;
  bool has_border_colour_inactive;
  struct cow_border_colour border_colour_inactive;

  /* Titlebar colors */
  bool has_titlebar_active;
  float titlebar_active_colour[4];
  bool has_titlebar_inactive;
  float titlebar_inactive_colour[4];
  bool has_titlebar_fg_active;
  float titlebar_fg_active[4];
  bool has_titlebar_fg_inactive;
  float titlebar_fg_inactive[4];
  bool has_titlebar_enabled;
  bool titlebar_enabled;
};

/*
 * The portable configuration struct for the river-based cow.
 *
 * Parsed from the line-based cow.conf format.  Only contains
 * settings that are relevant to the river window manager (no
 * compositor-specific settings like output backgrounds, input
 * device configs, or keyboard keymaps).
 */
struct cow_config {
  /* UI settings */
  int border;
  int gap;
  int step;
  int corner_length;
  bool handles; /* true = draw handle marks on borders */
  enum cow_border_style border_style;
  enum cow_placement_mode placement;

  /* Desktop configuration mode */
  enum {
    COW_DESKTOP_GLOBAL,
    COW_DESKTOP_PER_OUTPUT,
    COW_DESKTOP_SHARED,
  } desktop_config;

  /* Colors */
  struct cow_border_colour border_active;
  struct cow_border_colour border_inactive;

  /* Snap attraction distance (0 = disabled) */
  int snap_distance;

  /* Focus mode */
  enum {
    COW_FOCUS_MODE_CLICK,
    COW_FOCUS_MODE_SLOPPY,
  } focus_mode;

  /* Font */
  char *font_name;

  /* Icon size for iconified (minimized) windows */
  int icon_size;

  /* Titlebar */
  struct cow_titlebar_config titlebar;

  /* Container tab strip */
  struct cow_container_config container;

  /* Reserved area: pixels to subtract from usable area for a bar/panel.
   * Fullscreen ignores this; maximize/snap/cascade respect it. */
  struct {
    int top;
    int bottom;
    int left;
    int right;
  } reserved;

  /* Output background colour (from outputs config) */
  float output_bg_colour[4];
  bool has_output_bg_colour;

  /* Hooks: shell commands run at specific lifecycle points */
  char *hook_restart; /* run before execvp on SIGUSR1 restart */

  /* Parsed binding lists */
  struct wl_list keyboard_bindings; /* cow_binding_entry.link */
  struct wl_list mouse_bindings;    /* cow_mouse_binding_entry.link */
  struct wl_list actions;           /* cow_action_entry.link */
  struct wl_list view_configs;      /* cow_view_config_entry.link */
};

extern struct cow_config *cow_config;

/*
 * Parse a vector button definition string.
 * Format: "N x1xy1@c1 x2xy2@c2 ..."
 */
bool cow_parse_vector_button_string(struct cow_vector_button *vb,
                                    const char *str);

/*
 * Allocate and initialize a config with defaults.
 */
struct cow_config *cow_config_create(void);

/*
 * Free a config and all its lists.
 */
void cow_config_destroy(struct cow_config *config);

/*
 * Resolve the default config file path.
 * Checks: $COW_CONFIG, $XDG_CONFIG_HOME/cow/cow.conf,
 * ~/.config/cow/cow.conf, /etc/cow/cow.conf.
 */
const char *cow_config_default_path(void);

/*
 * Find a view config entry by app_id. Returns NULL if not found.
 */
struct cow_view_config_entry *cow_config_find_view(struct cow_config *config,
                                                   const char *app_id,
                                                   const char *title,
                                                   struct cow_output *output,
                                                   struct cow_desk *desk);

/*
 * Find an action entry by name. Returns the command string, or NULL.
 */
const char *cow_config_find_action(struct cow_config *config, const char *name);

#endif
