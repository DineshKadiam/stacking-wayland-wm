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
 * Named popup menus, layout, rendering, and input handling.
 */

#include <limits.h>
#include <pango/pangocairo.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include "cow/border.h"
#include "cow/cmd.h"
#include "cow/colour.h"
#include "cow/config.h"
#include "cow/draw.h"
#include "cow/ipc.h"
#include "cow/memory.h"
#include "cow/menu.h"
#include "cow/output.h"
#include "cow/seat.h"
#include "cow/shm.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

void
cow_menu_style_default(struct cow_menu_style *s)
{
	if (s == NULL)
		return;
	cow_colour_convert(s->bg, 0xbebebe);
	s->fg[0]	= 0.00f;
	s->fg[1]	= 0.00f;
	s->fg[2]	= 0.00f;
	s->fg[3]	= 1.0f;
	cow_colour_convert(s->title_bg, 0xbebebe);
	s->title_fg[0] = 0.00f;
	s->title_fg[1] = 0.00f;
	s->title_fg[2] = 0.00f;
	s->title_fg[3] = 1.0f;
	s->separator[0] = 0.40f;
	s->separator[1] = 0.40f;
	s->separator[2] = 0.40f;
	s->separator[3] = 1.0f;
	cow_colour_convert(s->border, 0x000000);
	s->bg_gradient.mode	       = COW_GRADIENT_NONE;
	s->title_bg_gradient.mode      = COW_GRADIENT_NONE;
	s->border_width		       = 0;
	s->padding_x		       = 6;
	s->padding_y		       = 2;
	s->title_underlines	       = 2;
	s->automatic_hotkeys	       = true;
	s->selection_relief	       = COW_MENU_SELECTION_RELIEF_RAISED;
}

static struct cow_menu_style g_default_style;
static bool		     g_default_style_inited;

static const struct cow_menu_style *
global_style(void)
{
	if (cow_config != NULL)
		return (&cow_config->menu_style);
	if (!g_default_style_inited) {
		cow_menu_style_default(&g_default_style);
		g_default_style_inited = true;
	}
	return (&g_default_style);
}

void
cow_menu_style_resolve(const struct cow_menu *menu, struct cow_menu_style *out)
{
	if (out == NULL)
		return;
	*out = *global_style();
	if (menu == NULL)
		return;
	uint32_t fields = menu->style_overrides;
#define APPLY(field, member)                                                   \
	do {                                                                     \
		if ((fields & (field)) != 0)                                      \
			memcpy(&out->member, &menu->style.member,                    \
			    sizeof(out->member));                                    \
	} while (0)
	APPLY(COW_MENU_STYLE_BG, bg);
	APPLY(COW_MENU_STYLE_FG, fg);
	APPLY(COW_MENU_STYLE_TITLE_BG, title_bg);
	APPLY(COW_MENU_STYLE_TITLE_FG, title_fg);
	APPLY(COW_MENU_STYLE_SEPARATOR, separator);
	APPLY(COW_MENU_STYLE_BORDER_COLOUR, border);
	APPLY(COW_MENU_STYLE_BG_GRADIENT, bg_gradient);
	APPLY(COW_MENU_STYLE_TITLE_BG_GRADIENT, title_bg_gradient);
	APPLY(COW_MENU_STYLE_BORDER_WIDTH, border_width);
	APPLY(COW_MENU_STYLE_PADDING_X, padding_x);
	APPLY(COW_MENU_STYLE_PADDING_Y, padding_y);
	APPLY(COW_MENU_STYLE_TITLE_UNDERLINES, title_underlines);
	APPLY(COW_MENU_STYLE_AUTOMATIC_HOTKEYS, automatic_hotkeys);
	APPLY(COW_MENU_STYLE_SELECTION_RELIEF, selection_relief);
#undef APPLY
}

static bool label_contains_folded(const char *, const char *);

static PangoFontDescription *g_font_desc;
static struct wl_keyboard   *g_menu_keyboard;
static struct xkb_keymap    *g_menu_keymap;
static struct xkb_state     *g_menu_xkb_state;
static bool		       g_menu_focus_requested;
static bool		       g_menu_restore_pending;
static struct cow_view      *g_menu_restore_view;
static char		      *g_deferred_action;
static char		      *g_deferred_context_id;
static uint32_t		       g_deferred_seat_name;
static bool		       g_deferred_pick_context;
static bool		       g_deferred_action_ready;

static void menu_navigate(const char *);

static void
menu_keyboard_keymap(__unused void *data, __unused struct wl_keyboard *keyboard,
    uint32_t format, int32_t fd, uint32_t size)
{
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return;
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (ctx == NULL) {
		munmap(map, size);
		return;
	}
	struct xkb_keymap *keymap = xkb_keymap_new_from_string(ctx, map,
	    XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	xkb_context_unref(ctx);
	if (keymap == NULL)
		return;
	if (g_menu_xkb_state != NULL)
		xkb_state_unref(g_menu_xkb_state);
	if (g_menu_keymap != NULL)
		xkb_keymap_unref(g_menu_keymap);
	g_menu_keymap = keymap;
	g_menu_xkb_state = xkb_state_new(keymap);
}

static void
menu_keyboard_enter(__unused void *data,
    __unused struct wl_keyboard *keyboard, __unused uint32_t serial,
    __unused struct wl_surface *surface, __unused struct wl_array *keys)
{
}

static void
menu_keyboard_leave(__unused void *data,
    __unused struct wl_keyboard *keyboard, __unused uint32_t serial,
    __unused struct wl_surface *surface)
{
}

static void
menu_keyboard_key(__unused void *data, __unused struct wl_keyboard *keyboard,
    __unused uint32_t serial, __unused uint32_t time, uint32_t key,
    uint32_t state)
{
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED ||
	    g_menu_xkb_state == NULL || !cow_menu_is_open())
		return;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(g_menu_xkb_state, key + 8);
	const char *direction = NULL;
	switch (sym) {
	case XKB_KEY_Up:
		direction = "up";
		break;
	case XKB_KEY_Down:
		direction = "down";
		break;
	case XKB_KEY_Left:
		direction = "left";
		break;
	case XKB_KEY_Right:
		direction = "right";
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		direction = "select";
		break;
	case XKB_KEY_Escape:
		direction = "close";
		break;
	case XKB_KEY_BackSpace:
		direction = "backspace";
		break;
	default:
		break;
	}
	if (direction != NULL) {
		menu_navigate(direction);
		return;
	}
	uint32_t ch = xkb_keysym_to_utf32(sym);
	if (ch < 128 && isalnum((unsigned char)ch)) {
		char text[2] = { (char)ch, '\0' };
		menu_navigate(text);
	}
}

static void
menu_keyboard_modifiers(__unused void *data,
    __unused struct wl_keyboard *keyboard, __unused uint32_t serial,
    uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
	if (g_menu_xkb_state != NULL)
		xkb_state_update_mask(g_menu_xkb_state, depressed, latched, locked,
		    0, 0, group);
}

static void
menu_keyboard_repeat_info(__unused void *data,
    __unused struct wl_keyboard *keyboard, __unused int32_t rate,
    __unused int32_t delay)
{
}

static const struct wl_keyboard_listener menu_keyboard_listener = {
	.keymap      = menu_keyboard_keymap,
	.enter       = menu_keyboard_enter,
	.leave       = menu_keyboard_leave,
	.key         = menu_keyboard_key,
	.modifiers   = menu_keyboard_modifiers,
	.repeat_info = menu_keyboard_repeat_info,
};

static void
menu_keyboard_destroy(void)
{
	if (g_menu_keyboard != NULL)
		wl_keyboard_release(g_menu_keyboard);
	if (g_menu_xkb_state != NULL)
		xkb_state_unref(g_menu_xkb_state);
	if (g_menu_keymap != NULL)
		xkb_keymap_unref(g_menu_keymap);
	g_menu_keyboard = NULL;
	g_menu_xkb_state = NULL;
	g_menu_keymap = NULL;
	g_menu_focus_requested = false;
}

static PangoFontDescription *
menu_font(void)
{
	if (g_font_desc != NULL)
		return (g_font_desc);
	const char *name = (cow_config != NULL && cow_config->font_name) ?
	    cow_config->font_name :
	    "Sans 11";
	g_font_desc	 = pango_font_description_from_string(name);
	/* If the font name had no explicit size, force 11pt. */
	if (pango_font_description_get_size(g_font_desc) == 0)
		pango_font_description_set_size(g_font_desc, 11 * PANGO_SCALE);
	return (g_font_desc);
}

/* Submenu hover dwell, in milliseconds. */
#define MENU_POPUP_DELAY_MS 200

static long
mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static uint32_t
px(const float c[4])
{
	return (cow_colour_to_pixel(c));
}

static unsigned int
pixel_brightness(uint32_t colour)
{
	return (2 * ((colour >> 16) & 0xff) +
	    3 * ((colour >> 8) & 0xff) + (colour & 0xff));
}

static void
menu_relief_colours(const float base[4], uint32_t *highlight, uint32_t *shadow)
{
	struct cow_colour_triple t;

	cow_colour_make_triple(&t, base);
	/*
	 * The Motif colour algorithm favours contrast for extreme colours: its
	 * "light" shade of a pale colour can be darker than the base, and its
	 * "dark" shade of a dark colour can be lighter.  That is useful in some
	 * contexts but makes a raised menu item look flat or sunken.  Preserve
	 * the traditional shades when they point in the expected direction and
	 * otherwise blend the offending edge towards white or black.
	 */
	unsigned int base_brightness = pixel_brightness(t.base);
	if (pixel_brightness(t.lt) <= base_brightness) {
		float lt[4] = {
		    base[0] + (1.0f - base[0]) * 0.5f,
		    base[1] + (1.0f - base[1]) * 0.5f,
		    base[2] + (1.0f - base[2]) * 0.5f,
		    base[3],
		};
		t.lt = cow_colour_to_pixel(lt);
	}
	if (pixel_brightness(t.dk) >= base_brightness) {
		float dk[4] = {
		    base[0] * 0.5f,
		    base[1] * 0.5f,
		    base[2] * 0.5f,
		    base[3],
		};
		t.dk = cow_colour_to_pixel(dk);
	}
	if (highlight != NULL)
		*highlight = t.lt;
	if (shadow != NULL)
		*shadow = t.dk;
}

/* Extra state keyed by popup pointer. */
struct popup_ext {
	struct cow_menu_popup *popup;
	long		       hover_started_ms;
	int		       hover_item; /* -1 = none */
	bool		       hover_arrow_pending;
};
#define POPUP_EXT_MAX 16
static struct popup_ext g_popup_ext[POPUP_EXT_MAX];
static bool		g_hover_suppressed_until_pointer_moves;

static struct popup_ext *
ext_for(struct cow_menu_popup *p, bool create)
{
	for (int i = 0; i < POPUP_EXT_MAX; i++) {
		if (g_popup_ext[i].popup == p)
			return (&g_popup_ext[i]);
	}
	if (!create)
		return (NULL);
	for (int i = 0; i < POPUP_EXT_MAX; i++) {
		if (g_popup_ext[i].popup == NULL) {
			g_popup_ext[i].popup		= p;
			g_popup_ext[i].hover_item	= -1;
			g_popup_ext[i].hover_started_ms = 0;
			return (&g_popup_ext[i]);
		}
	}
	return (NULL);
}

static void
ext_release(struct cow_menu_popup *p)
{
	struct popup_ext *e = ext_for(p, false);
	if (e != NULL)
		memset(e, 0, sizeof(*e));
}

static void
hover_reset_all(void)
{
	for (int i = 0; i < POPUP_EXT_MAX; i++) {
		g_popup_ext[i].hover_started_ms = 0;
		g_popup_ext[i].hover_item	= -1;
	}
}

static int
hotkey_fold(int ch)
{
	return (tolower((unsigned char)ch));
}

static bool
hotkey_usable(int ch)
{
	return (isalnum((unsigned char)ch) != 0);
}

static int
label_explicit_hotkey(const char *label)
{
	if (label == NULL)
		return (-1);
	for (const char *p = label; *p != '\0'; p++) {
		if (*p != '&' || p[1] == '\0')
			continue;
		p++;
		if (*p == '&')
			continue;
		return (hotkey_fold((unsigned char)*p));
	}
	return (-1);
}

static bool
menu_item_can_have_hotkey(const struct cow_menu_item *it)
{
	return (it != NULL && it->type == COW_MENU_ITEM_NORMAL &&
	    it->label != NULL);
}

static int
menu_auto_hotkey_for(const struct cow_menu *menu,
    const struct cow_menu_item *target)
{
	struct cow_menu_style style;
	cow_menu_style_resolve(menu, &style);
	if (menu == NULL || target == NULL || !style.automatic_hotkeys)
		return (-1);
	int explicit_key = label_explicit_hotkey(target->label);
	if (explicit_key >= 0)
		return (explicit_key);
	if (menu->typeahead)
		return (-1);

	bool used[256] = { false };
	struct cow_menu_item *it;
	wl_list_for_each(it, &menu->items, link)
	{
		if (!menu_item_can_have_hotkey(it))
			continue;
		int key = label_explicit_hotkey(it->label);
		if (key >= 0)
			used[(unsigned char)key] = true;
	}

	wl_list_for_each(it, &menu->items, link)
	{
		if (!menu_item_can_have_hotkey(it))
			continue;
		int key = label_explicit_hotkey(it->label);
		if (key < 0) {
			for (const char *p = it->label; *p != '\0'; p++) {
				if (*p == '&' && p[1] == '&') {
					p++;
					continue;
				}
				int c = hotkey_fold((unsigned char)*p);
				if (!hotkey_usable(c) || used[(unsigned char)c])
					continue;
				key = c;
				used[(unsigned char)c] = true;
				break;
			}
		}
		if (it == target)
			return (key);
	}
	return (-1);
}

static void
menu_label_resolve(const struct cow_menu *menu, const struct cow_menu_item *it,
    char *out, size_t outlen, int *underline_out)
{
	if (outlen == 0)
		return;
	out[0] = '\0';
	if (underline_out != NULL)
		*underline_out = -1;
	if (it == NULL || it->label == NULL)
		return;

	int explicit_key = label_explicit_hotkey(it->label);
	int auto_key = explicit_key >= 0 ? explicit_key :
	    menu_auto_hotkey_for(menu, it);
	bool used_explicit = false;
	bool used_auto = false;
	size_t bi = 0;

	for (const char *p = it->label; *p != '\0' && bi + 1 < outlen; p++) {
		if (*p == '&' && p[1] != '\0') {
			p++;
			if (*p != '&' && !used_explicit) {
				if (underline_out != NULL)
					*underline_out = (int)bi;
				used_explicit = true;
			}
		}
		int c = (unsigned char)*p;
		if (explicit_key < 0 && auto_key >= 0 && !used_auto &&
		    hotkey_fold(c) == auto_key) {
			if (underline_out != NULL)
				*underline_out = (int)bi;
			used_auto = true;
		}
		out[bi++] = (char)c;
	}
	out[bi] = '\0';
}

struct cow_menu *
cow_menu_find(const char *name)
{
	if (name == NULL)
		return (NULL);
	struct cow_menu *m;
	wl_list_for_each(m, &cow_wm.menus, link)
	{
		if (m->name != NULL && strcmp(m->name, name) == 0)
			return (m);
	}
	return (NULL);
}

struct cow_menu *
cow_menu_create(const char *name)
{
	if (name == NULL || name[0] == '\0')
		return (NULL);
	if (cow_menu_find(name) != NULL)
		return (NULL);
	struct cow_menu *m = xcalloc(1, sizeof(*m));
	m->name		   = xstrdup(name);
	wl_list_init(&m->items);
	wl_list_insert(cow_wm.menus.prev, &m->link);
	return (m);
}

static void
menu_item_free(struct cow_menu_item *it)
{
	xfree(it->label);
	xfree(it->action);
	xfree(it);
}

void
cow_menu_clear(struct cow_menu *menu)
{
	if (menu == NULL)
		return;
	struct cow_menu_item *it, *tmp;
	wl_list_for_each_safe(it, tmp, &menu->items, link)
	{
		wl_list_remove(&it->link);
		menu_item_free(it);
	}
}

/* Forward decl. */
static void popup_destroy(struct cow_menu_popup *p);

static void
close_popups_of(const struct cow_menu *def)
{
	struct cow_menu_popup *p, *tmp;
	wl_list_for_each_safe(p, tmp, &cow_wm.open_menus, link)
	{
		if (p->def == def)
			cow_menu_popup_close(p);
	}
}

void
cow_menu_destroy(struct cow_menu *menu)
{
	if (menu == NULL)
		return;
	close_popups_of(menu);
	cow_menu_clear(menu);
	wl_list_remove(&menu->link);
	xfree(menu->name);
	xfree(menu);
}

void
cow_menu_destroy_all(void)
{
	struct cow_menu *m, *tmp;
	if (cow_wm.menus.next != NULL) {
		wl_list_for_each_safe(m, tmp, &cow_wm.menus, link)
		    cow_menu_destroy(m);
	}
	cow_menu_close_all();
	menu_keyboard_destroy();
	xfree(g_deferred_action);
	xfree(g_deferred_context_id);
	g_deferred_action = NULL;
	g_deferred_context_id = NULL;
	g_deferred_action_ready = false;
	if (g_font_desc != NULL) {
		pango_font_description_free(g_font_desc);
		g_font_desc = NULL;
	}
}

struct cow_menu_item *
cow_menu_item_add(struct cow_menu *menu, enum cow_menu_item_type type,
    const char *label, const char *action, bool is_popup,
    bool popup_pick_context)
{
	if (menu == NULL)
		return (NULL);
	struct cow_menu_item *it = xcalloc(1, sizeof(*it));
	it->type		 = type;
	it->label		 = (label != NULL) ? xstrdup(label) : NULL;
	it->action		 = (action != NULL) ? xstrdup(action) : NULL;
	it->is_popup		 = is_popup;
	it->popup_pick_context	 = popup_pick_context;
	wl_list_insert(menu->items.prev, &it->link);
	return (it);
}

void
cow_menu_enable_numbered_selection(struct cow_menu *menu)
{
	if (menu == NULL)
		return;
	menu->numbered = true;
}

void
cow_menu_enable_typeahead(struct cow_menu *menu)
{
	if (menu == NULL)
		return;
	menu->typeahead = true;
	menu->typeahead_query[0] = '\0';
}

static int
item_row_height(const struct cow_menu_item *it, int text_h,
    const struct cow_menu_style *st)
{
	switch (it->type) {
	case COW_MENU_ITEM_SEPARATOR:
		return (2);
	case COW_MENU_ITEM_TITLE:
	case COW_MENU_ITEM_NORMAL:
	default:
		return (text_h + st->padding_y * 2);
	}
}

static bool
menu_item_visible(const struct cow_menu *menu, const struct cow_menu_item *it)
{
	if (menu == NULL || it == NULL || !menu->typeahead ||
	    menu->typeahead_query[0] == '\0')
		return (true);
	if (it->type != COW_MENU_ITEM_NORMAL)
		return (true);
	return (label_contains_folded(it->label, menu->typeahead_query));
}

static void
text_extents(const struct cow_menu *menu, const struct cow_menu_item *it,
    bool bold, int *w_out, int *h_out)
{
	*w_out = 0;
	*h_out = 0;
	if (it == NULL || it->label == NULL || it->label[0] == '\0')
		return;
	cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
	    1, 1);
	cairo_t		*cr   = cairo_create(surf);
	PangoLayout	*lay  = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_copy(menu_font());
	if (bold)
		pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
	pango_layout_set_font_description(lay, fd);
	pango_font_description_free(fd);

	char clean[512];
	int  underline;
	menu_label_resolve(menu, it, clean, sizeof(clean), &underline);
	pango_layout_set_text(lay, clean, -1);
	int w, h;
	pango_layout_get_pixel_size(lay, &w, &h);
	g_object_unref(lay);
	cairo_destroy(cr);
	cairo_surface_destroy(surf);
	*w_out = w;
	*h_out = h;
}

static void
popup_layout(struct cow_menu_popup *p)
{
	const struct cow_menu_style *st = &p->style;
	const int ARROW_W = 14;

	int		      max_label_w = 0;
	int		      max_text_h  = 0;
	bool		      any_submenu = false;
	struct cow_menu_item *it;
	wl_list_for_each(it, &p->def->items, link)
	{
		if (!menu_item_visible(p->def, it))
			continue;
		if (it->type == COW_MENU_ITEM_SEPARATOR)
			continue;
		int tw, th;
		text_extents(p->def, it, it->type == COW_MENU_ITEM_TITLE, &tw,
		    &th);
		if (tw > max_label_w)
			max_label_w = tw;
		if (th > max_text_h)
			max_text_h = th;
		if (it->is_popup)
			any_submenu = true;
	}
	if (max_text_h <= 0)
		max_text_h = 12;
	/* Always reserve the arrow gutter so columns align whether or
	 * not any one item is a submenu link. */
	(void)any_submenu;
	int text_w = max_label_w + ARROW_W;

	int inner_w = text_w + st->padding_x * 2;
	int height  = st->border_width * 2;
	wl_list_for_each(it, &p->def->items, link)
	{
		if (!menu_item_visible(p->def, it))
			continue;
		height += item_row_height(it, max_text_h, st);
	}

	p->item_h = max_text_h + st->padding_y * 2;
	p->width  = inner_w + st->border_width * 2;
	p->height = height;
}

static struct cow_output *
popup_output_for(const struct cow_menu_popup *parent, int x, int y)
{
	struct cow_output *out = NULL;

	if (parent != NULL) {
		out = cow_output_at(parent->x, parent->y);
		if (out == NULL)
			out = cow_output_at(parent->x + parent->width / 2,
			    parent->y + parent->height / 2);
	}
	if (out == NULL)
		out = cow_output_at(x, y);
	if (out == NULL)
		out = cow_wm.current_output;

	return (out);
}

static void
popup_shift_chain(struct cow_menu_popup *popup, int dx)
{
	for (struct cow_menu_popup *p = popup; p != NULL; p = p->parent)
		p->x += dx;
}

static int
popup_chain_left(const struct cow_menu_popup *popup)
{
	int left = INT_MAX;

	for (const struct cow_menu_popup *p = popup; p != NULL; p = p->parent) {
		if (p->x < left)
			left = p->x;
	}

	return (left == INT_MAX ? 0 : left);
}

static void
popup_fit_to_output(struct cow_menu_popup *popup, int *x, int *y,
    struct cow_menu_popup *parent)
{
	struct cow_output *out = popup_output_for(parent, *x, *y);
	if (out == NULL)
		return;

	int right  = out->x + out->width;
	int bottom = out->y + out->height;

	if (parent != NULL && *x + popup->width > right) {
		int shift = *x + popup->width - right;
		popup_shift_chain(parent, -shift);
		*x -= shift;

		int left = popup_chain_left(parent);
		if (left < out->x) {
			int correction = out->x - left;
			popup_shift_chain(parent, correction);
			*x += correction;
		}
	}

	if (*x + popup->width > right)
		*x = right - popup->width;
	if (*x < out->x)
		*x = out->x;
	if (*y + popup->height > bottom)
		*y = bottom - popup->height;
	if (*y < out->y)
		*y = out->y;
}

/*
 * Render `text` inside the rect (x,y,w,h) on cairo surface `cs`.
 *
 *   align == 0   left-align (default)
 *   align == 1   centre horizontally
 *
 * A literal `&` in the text marks the following character as the
 * keyboard hot-key: the `&` is stripped and the marked character is
 * rendered underlined.
 */
static void
render_text(cairo_surface_t *cs, int x, int y, int w, int h, const char *text,
    int underline, const float fg[4], bool bold, int align)
{
	if (text == NULL || text[0] == '\0')
		return;
	cairo_t *cr = cairo_create(cs);
	cairo_rectangle(cr, x, y, w, h);
	cairo_clip(cr);
	cairo_set_source_rgba(cr, fg[0], fg[1], fg[2], fg[3]);
	PangoLayout	     *lay = pango_cairo_create_layout(cr);
	PangoFontDescription *fd  = pango_font_description_copy(menu_font());
	if (bold)
		pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
	pango_layout_set_font_description(lay, fd);
	pango_font_description_free(fd);

	if (underline >= 0) {
		char	    buf[512];
		size_t	    bi		= 0;
		const char *src		= text;
		int	    idx		= 0;
		while (*src != '\0' && bi + 16 < sizeof(buf)) {
			if (idx == underline) {
				if (*src == '<') {
					bi += snprintf(buf + bi, sizeof(buf) - bi,
					    "<u>&lt;</u>");
				} else if (*src == '>') {
					bi += snprintf(buf + bi, sizeof(buf) - bi,
					    "<u>&gt;</u>");
				} else if (*src == '&') {
					bi += snprintf(buf + bi, sizeof(buf) - bi,
					    "<u>&amp;</u>");
				} else {
					bi += snprintf(buf + bi, sizeof(buf) - bi,
					    "<u>%c</u>", (unsigned char)*src);
				}
				src++;
				idx++;
				continue;
			}
			if (*src == '<') {
				bi += snprintf(buf + bi, sizeof(buf) - bi,
				    "&lt;");
			} else if (*src == '>') {
				bi += snprintf(buf + bi, sizeof(buf) - bi,
				    "&gt;");
			} else if (*src == '&') {
				bi += snprintf(buf + bi, sizeof(buf) - bi,
				    "&amp;");
			} else {
				buf[bi++] = *src;
			}
			src++;
			idx++;
		}
		buf[bi] = '\0';
		pango_layout_set_markup(lay, buf, -1);
	} else {
		pango_layout_set_text(lay, text, -1);
	}

	int tw, th;
	pango_layout_get_pixel_size(lay, &tw, &th);
	int tx = x;
	if (align == 1)
		tx = x + (w - tw) / 2;
	int ty = y + (h - th) / 2;
	cairo_move_to(cr, tx, ty);
	pango_cairo_show_layout(cr, lay);
	g_object_unref(lay);
	cairo_destroy(cr);
}

static void
fill_style_rect(struct cow_pixbuf *buf, int x, int y, int w, int h,
    const float colour[4], const struct cow_gradient *gradient)
{
	cow_draw_fill_rect(buf, x, y, w, h, px(colour));
	if (gradient != NULL && gradient->mode != COW_GRADIENT_NONE)
		cow_draw_gradient_rect(buf, x, y, w, h, gradient);
}

static void
draw_submenu_arrow(struct cow_pixbuf *buf, int x, int y, int size,
    bool depressed)
{
	if (buf == NULL || buf->pixels == NULL)
		return;
	if (depressed) {
		x++;
		y++;
	}

	uint32_t fill = 0;
	if (x >= 0 && x < buf->width && y >= 0 && y < buf->height)
		fill = buf->pixels[y * buf->stride_px + x];
	float base[4];
	struct cow_colour_triple arrow_colours;
	cow_colour_convert(base, fill);
	cow_colour_make_triple(&arrow_colours, base);

	for (int i = 0; i <= size; i++) {
		int half = size - i;
		cow_draw_vline(buf, x + i, y - half, y + half, fill);
	}

	uint32_t top_left = depressed ? arrow_colours.dk : arrow_colours.lt;
	uint32_t bottom_right = depressed ? arrow_colours.lt : arrow_colours.dk;
	cow_draw_vline(buf, x, y - size, y + size, top_left);
	for (int i = 0; i <= size; i++) {
		cow_draw_hline(buf, x + i, x + i, y - size + i, top_left);
		cow_draw_hline(buf, x + i, x + i, y + size - i,
		    bottom_right);
	}
}

static void
popup_render(struct cow_menu_popup *p)
{
	if (p == NULL || p->buffer == NULL || p->buffer->data == NULL)
		return;

	const struct cow_menu_style *st	 = &p->style;
	struct cow_pixbuf	     buf = {
			   .pixels    = p->buffer->data,
			   .stride_px = p->buffer->stride / 4,
			   .width     = p->width,
			   .height    = p->height,
	};

	uint32_t hl_px, sh_px;
	menu_relief_colours(st->bg, &hl_px, &sh_px);
	const uint32_t sep_px    = px(st->separator);
	const uint32_t border_px = px(st->border);

	fill_style_rect(&buf, 0, 0, p->width, p->height, st->bg,
	    &st->bg_gradient);

	if (st->border_width > 0) {
		int bw = st->border_width;
		cow_draw_fill_rect(&buf, 0, 0, p->width, bw, border_px);
		cow_draw_fill_rect(&buf, 0, p->height - bw, p->width, bw,
		    border_px);
		cow_draw_fill_rect(&buf, 0, bw, bw, p->height - bw * 2,
		    border_px);
		cow_draw_fill_rect(&buf, p->width - bw, bw, bw,
		    p->height - bw * 2, border_px);
	}

	int		 y	= st->border_width;
	int		 x	= st->border_width;
	int		 innerw = p->width - st->border_width * 2;
	int		 idx	= 0;
	cairo_surface_t *cs =
	    cairo_image_surface_create_for_data((unsigned char *)buf.pixels,
		CAIRO_FORMAT_ARGB32, p->width, p->height, p->buffer->stride);

	/* Keep submenu arrows aligned across rows. */
	const int ARROW_GUTTER = 14;
	const int ARROW_SIZE   = 4;

	struct cow_menu_item *it;
	wl_list_for_each(it, &p->def->items, link)
	{
		if (!menu_item_visible(p->def, it)) {
			idx++;
			continue;
		}
		int rh = item_row_height(it, p->item_h - st->padding_y * 2, st);

		switch (it->type) {
		case COW_MENU_ITEM_SEPARATOR: {
			cow_draw_hline(&buf, x + 2, x + innerw - 3, y, sh_px);
			cow_draw_hline(&buf, x + 2, x + innerw - 3, y + 1,
			    hl_px);
			break;
		}
		case COW_MENU_ITEM_TITLE:
			fill_style_rect(&buf, x, y, innerw, rh, st->title_bg,
			    &st->title_bg_gradient);
			{
			char label[512];
			int  underline;
			menu_label_resolve(p->def, it, label, sizeof(label),
			    &underline);
			if (p->def->typeahead &&
			    p->def->typeahead_query[0] != '\0') {
				snprintf(label, sizeof(label), "Search: %s",
				    p->def->typeahead_query);
				underline = -1;
			}
			render_text(cs, x + st->padding_x, y,
			    innerw - st->padding_x * 2, rh - 2, label,
			    underline,
			    st->title_fg, true, 1 /* centre */);
			}
			for (int u = 0; u < st->title_underlines && u < 2; u++) {
				int yy = y + rh - 2 - u * 2;
				cow_draw_hline(&buf, x + 2, x + innerw - 3, yy,
				    sep_px);
			}
			break;
		case COW_MENU_ITEM_NORMAL:
		default: {
			bool hovered = (idx == p->selected);
			bool open_child = (hovered && it->is_popup &&
			    p->child != NULL);
			if (hovered) {
				if (st->selection_relief ==
				    COW_MENU_SELECTION_RELIEF_RAISED) {
					cow_draw_relieve_rect(&buf, x + 1, y,
					    innerw - 2, rh, 2,
					    hl_px, hl_px, sh_px, sh_px);
				} else {
					cow_draw_fill_rect(&buf, x, y, innerw, rh,
					    hl_px);
				}
			}

			int label_w = innerw - st->padding_x * 2 - ARROW_GUTTER;
			if (label_w < 1)
				label_w = 1;
			char label[512];
			int  underline;
			menu_label_resolve(p->def, it, label, sizeof(label),
			    &underline);
			render_text(cs, x + st->padding_x, y, label_w, rh,
			    label, underline, st->fg, false, 0);

			if (it->is_popup) {
				int cy = y + rh / 2;
				int cx = x + innerw - ARROW_GUTTER / 2 -
				    ARROW_SIZE / 2;
				draw_submenu_arrow(&buf, cx, cy, ARROW_SIZE,
				    open_child);
			}
			break;
		}
		}

		y += rh;
		idx++;
	}
	cow_draw_relieve_rect(&buf, 0, 0, p->width, p->height, 1,
	    sep_px, sep_px, sep_px, sep_px);
	cairo_surface_destroy(cs);
}

void
cow_menu_popup_redraw(struct cow_menu_popup *popup)
{
	if (popup == NULL)
		return;
	popup->needs_redraw	  = true;
	cow_wm.needs_manage_dirty = true;
}

void
cow_menu_style_refresh(struct cow_menu *menu, bool geometry_changed)
{
	if (!cow_menu_is_open())
		return;
	struct cow_menu_popup *p;
	wl_list_for_each(p, &cow_wm.open_menus, link)
	{
		if (menu == NULL || p->def == menu) {
			cow_menu_style_resolve(p->def, &p->style);
			p->needs_redraw = true;
		}
		if (geometry_changed)
			p->needs_layout = true;
	}
	cow_wm.needs_manage_dirty = true;
}

static struct cow_menu_popup *
popup_alloc(struct cow_menu *def)
{
	struct cow_menu_popup *p = xcalloc(1, sizeof(*p));
	p->def			 = def;
	p->selected		 = -1;
	p->child_item		 = -1;
	cow_menu_style_resolve(def, &p->style);
	return (p);
}

static void
popup_destroy(struct cow_menu_popup *p)
{
	if (p == NULL)
		return;
	if (p->buffer != NULL) {
		cow_shm_buffer_destroy(p->buffer);
		p->buffer = NULL;
	}
	if (p->node != NULL) {
		river_node_v1_destroy(p->node);
		p->node = NULL;
	}
	if (p->shell_surface != NULL) {
		river_shell_surface_v1_destroy(p->shell_surface);
		p->shell_surface = NULL;
	}
	if (p->surface != NULL) {
		wl_surface_destroy(p->surface);
		p->surface = NULL;
	}
	ext_release(p);
	xfree(p);
}

void
cow_menu_popup_close(struct cow_menu_popup *popup)
{
	if (popup == NULL)
		return;
	if (popup->child != NULL) {
		cow_menu_popup_close(popup->child);
		popup->child = NULL;
	}
	if (popup->parent != NULL && popup->parent->child == popup)
		popup->parent->child = NULL;
	if (popup->parent != NULL && popup->parent->child_item >= 0)
		popup->parent->child_item = -1;
	if (popup->parent != NULL) {
		popup->parent->needs_redraw = true;
		cow_wm.needs_manage_dirty   = true;
	}

	/* Defer surface/node teardown to the render hook. */
	wl_list_remove(&popup->link);
	popup->needs_destroy = true;
	wl_list_insert(cow_wm.menu_graveyard.prev, &popup->link);
	cow_wm.needs_manage_dirty = true;
}

void
cow_menu_close_all(void)
{
	if (cow_wm.open_menus.next == NULL)
		return;
	while (!wl_list_empty(&cow_wm.open_menus)) {
		struct cow_menu_popup *p =
		    wl_container_of(cow_wm.open_menus.next, p, link);
		cow_menu_popup_close(p);
	}
	cow_wm.menu_context_view = NULL;
	cow_wm.menu_context_seat = NULL;
	menu_keyboard_destroy();
	g_menu_restore_pending = true;
	g_hover_suppressed_until_pointer_moves = false;
	hover_reset_all();
	cow_wm.needs_manage_dirty = true;
}

void
cow_menu_apply_focus_restore(void)
{
	if (!g_menu_restore_pending || cow_menu_is_open())
		return;
	g_menu_restore_pending = false;
	struct cow_seat *seat = cow_seat_primary();
	if (cow_wm.pending_focus == NULL && g_menu_restore_view != NULL &&
	    !g_menu_restore_view->closed &&
	    cow_wm.focused_view == g_menu_restore_view && seat != NULL)
		cow_seat_focus_view_no_raise(seat, g_menu_restore_view);
	g_menu_restore_view = NULL;
}

bool
cow_menu_is_open(void)
{
	return (cow_wm.open_menus.next != NULL &&
	    !wl_list_empty(&cow_wm.open_menus));
}

struct cow_menu_popup *
cow_menu_popup_open(struct cow_menu *menu, int x, int y,
    struct cow_view *context_view, struct cow_menu_popup *parent,
    bool centre_title_on_position)
{
	if (menu == NULL)
		return (NULL);
	if (cow_wm.compositor == NULL || cow_wm.manager == NULL)
		return (NULL);

	struct cow_menu_popup *p = popup_alloc(menu);
	p->parent		 = parent;
	if (parent != NULL) {
		if (parent->child != NULL)
			cow_menu_popup_close(parent->child);
		parent->child = p;
		parent->child_item = parent->selected;
		parent->needs_redraw = true;
	} else {
		g_hover_suppressed_until_pointer_moves = false;
		hover_reset_all();
		cow_wm.menu_context_view = context_view;
		cow_wm.menu_context_seat = cow_wm.current_interaction_seat;
		if (cow_wm.menu_context_seat == NULL)
			cow_wm.menu_context_seat = cow_seat_primary();
		g_menu_restore_view = cow_wm.focused_view;
		g_menu_restore_pending = false;
		g_menu_focus_requested = false;
	}

	popup_layout(p);

	if (parent == NULL && centre_title_on_position) {
		x -= p->width / 2;
		y -= p->style.border_width + p->item_h / 2;
	}
	popup_fit_to_output(p, &x, &y, parent);
	p->x = x;
	p->y = y;

	wl_list_insert(cow_wm.open_menus.prev, &p->link);
	(void)ext_for(p, true);

	p->needs_create		  = true;
	cow_wm.needs_manage_dirty = true;

	return (p);
}

struct cow_menu_popup *
cow_menu_popup_open_centered(struct cow_menu *menu, int cx, int cy,
    struct cow_view *context_view)
{
	if (menu == NULL)
		return (NULL);
	struct cow_menu_popup *p = popup_alloc(menu);
	g_hover_suppressed_until_pointer_moves = false;
	hover_reset_all();
	cow_wm.menu_context_view = context_view;
	cow_wm.menu_context_seat = cow_wm.current_interaction_seat;
	if (cow_wm.menu_context_seat == NULL)
		cow_wm.menu_context_seat = cow_seat_primary();
	g_menu_restore_view = cow_wm.focused_view;
	g_menu_restore_pending = false;
	g_menu_focus_requested = false;
	menu->typeahead_query[0] = '\0';
	p->keep_centered = true;

	popup_layout(p);
	int x = cx - p->width / 2;
	int y = cy - p->height / 2;
	popup_fit_to_output(p, &x, &y, NULL);
	p->x = x;
	p->y = y;

	wl_list_insert(cow_wm.open_menus.prev, &p->link);
	(void)ext_for(p, true);
	p->needs_create = true;
	cow_wm.needs_manage_dirty = true;
	return (p);
}

static void
popup_materialise(struct cow_menu_popup *p)
{
	if (p == NULL || !p->needs_create)
		return;
	p->needs_create = false;

	p->surface = wl_compositor_create_surface(cow_wm.compositor);
	if (p->surface == NULL)
		return;
	p->shell_surface = river_window_manager_v1_get_shell_surface(
	    cow_wm.manager, p->surface);
	if (p->shell_surface == NULL)
		return;
	p->node = river_shell_surface_v1_get_node(p->shell_surface);

	p->buffer = cow_shm_buffer_create(p->width, p->height);
	if (p->buffer == NULL)
		return;

	popup_render(p);
	river_node_v1_set_position(p->node, p->x, p->y);
	river_node_v1_place_top(p->node);

	wl_surface_attach(p->surface, p->buffer->buffer, 0, 0);
	wl_surface_damage_buffer(p->surface, 0, 0, p->width, p->height);
	river_shell_surface_v1_sync_next_commit(p->shell_surface);
	wl_surface_commit(p->surface);
	p->buffer->busy = true;
	p->committed	= true;
	if (p->parent == NULL)
		cow_wm_request_manage_dirty();
}

static void
popup_recommit(struct cow_menu_popup *p)
{
	if (p == NULL || p->buffer == NULL || p->surface == NULL ||
	    p->shell_surface == NULL)
		return;
	popup_render(p);
	wl_surface_attach(p->surface, p->buffer->buffer, 0, 0);
	wl_surface_damage_buffer(p->surface, 0, 0, p->width, p->height);
	river_shell_surface_v1_sync_next_commit(p->shell_surface);
	wl_surface_commit(p->surface);
	p->buffer->busy = true;
}

static void
popup_apply_relayout(struct cow_menu_popup *p)
{
	if (p == NULL || !p->needs_layout)
		return;
	int old_width = p->width;
	int old_height = p->height;
	int old_item_h = p->item_h;
	int old_centre_x = p->x + old_width / 2;
	int old_centre_y = p->y + old_height / 2;
	popup_layout(p);
	if (p->buffer != NULL &&
	    (p->buffer->width != p->width || p->buffer->height != p->height)) {
		struct cow_shm_buffer *replacement =
		    cow_shm_buffer_create(p->width, p->height);
		if (replacement == NULL) {
			p->width = old_width;
			p->height = old_height;
			p->item_h = old_item_h;
			return;
		}
		cow_shm_buffer_destroy(p->buffer);
		p->buffer = replacement;
	}
	if (p->keep_centered) {
		p->x = old_centre_x - p->width / 2;
		p->y = old_centre_y - p->height / 2;
	}
	p->needs_layout = false;
	p->needs_redraw = true;
}

static void
popup_refit_position(struct cow_menu_popup *p)
{
	if (p == NULL)
		return;
	if (p->parent == NULL) {
		int x = p->x;
		int y = p->y;
		popup_fit_to_output(p, &x, &y, NULL);
		p->x = x;
		p->y = y;
		return;
	}
	struct cow_menu_popup *parent = p->parent;
	const struct cow_menu_style *st = &parent->style;
	int y = parent->y + st->border_width;
	int idx = 0;
	struct cow_menu_item *it;
	wl_list_for_each(it, &parent->def->items, link)
	{
		if (idx >= parent->child_item)
			break;
		if (menu_item_visible(parent->def, it))
			y += item_row_height(it,
			    parent->item_h - st->padding_y * 2, st);
		idx++;
	}
	int x = parent->x + parent->width - 5;
	popup_fit_to_output(p, &x, &y, parent);
	p->x = x;
	p->y = y;
}

void
cow_menu_render_all(void)
{
	bool destroyed = false;

	/* Destroy popups whose close was deferred to the render hook. */
	if (cow_wm.menu_graveyard.next != NULL) {
		struct cow_menu_popup *gp, *gtmp;
		wl_list_for_each_safe(gp, gtmp, &cow_wm.menu_graveyard, link)
		{
			wl_list_remove(&gp->link);
			popup_destroy(gp);
			destroyed = true;
		}
	}
	if (destroyed && g_deferred_action != NULL) {
		g_deferred_action_ready = true;
		cow_wm_request_manage_dirty();
	}

	if (!cow_menu_is_open())
		return;
	struct cow_menu_popup *p;
	wl_list_for_each(p, &cow_wm.open_menus, link)
	{
		popup_apply_relayout(p);
	}
	wl_list_for_each(p, &cow_wm.open_menus, link)
	{
		popup_refit_position(p);
	}
	wl_list_for_each(p, &cow_wm.open_menus, link)
	{
		if (p->needs_create) {
			popup_materialise(p);
			continue;
		}
		if (p->needs_redraw) {
			popup_recommit(p);
			p->needs_redraw = false;
		}
		if (p->node != NULL) {
			river_node_v1_set_position(p->node, p->x, p->y);
			river_node_v1_place_top(p->node);
		}
	}
}

/* Find which popup the pointer is over (deepest-child first). */
static struct cow_menu_popup *
popup_under(int px_x, int px_y)
{
	struct cow_menu_popup *match = NULL;
	struct cow_menu_popup *p;
	wl_list_for_each(p, &cow_wm.open_menus, link)
	{
		if (px_x >= p->x && px_x < p->x + p->width && px_y >= p->y &&
		    px_y < p->y + p->height) {
			match = p; /* keep going to prefer later (child) */
		}
	}
	return (match);
}

/* Return the item under the pointer, or -1. */
static int
item_under(const struct cow_menu_popup *p, int px_x, int px_y)
{
	const struct cow_menu_style *st	    = &p->style;
	int			     x	    = p->x + st->border_width;
	int			     innerw = p->width - st->border_width * 2;
	if (px_x < x || px_x >= x + innerw)
		return (-1);
	int		      y	  = p->y + st->border_width;
	int		      idx = 0;
	struct cow_menu_item *it;
	wl_list_for_each(it, &p->def->items, link)
	{
		if (!menu_item_visible(p->def, it)) {
			idx++;
			continue;
		}
		int rh = item_row_height(it, p->item_h - st->padding_y * 2, st);
		if (px_y >= y && px_y < y + rh)
			return (idx);
		y += rh;
		idx++;
	}
	return (-1);
}

static struct cow_menu_item *
item_at_index(const struct cow_menu *def, int idx)
{
	int		      i = 0;
	struct cow_menu_item *it;
	wl_list_for_each(it, &def->items, link)
	{
		if (i == idx)
			return (it);
		i++;
	}
	return (NULL);
}

static bool
item_selectable(const struct cow_menu_item *it)
{
	return (it != NULL && it->type == COW_MENU_ITEM_NORMAL);
}

static bool
popup_is_ancestor_of(const struct cow_menu_popup *ancestor,
    const struct cow_menu_popup *popup)
{
	for (const struct cow_menu_popup *p = popup; p != NULL; p = p->parent) {
		if (p == ancestor)
			return (true);
	}
	return (false);
}

static void
popup_close_child_if_unrelated(struct cow_menu_popup *p,
    struct cow_menu_popup *over)
{
	if (p == NULL || p->needs_destroy || p->child == NULL)
		return;
	if (over == NULL)
		return;
	if (popup_is_ancestor_of(p->child, over))
		return;
	if (over == p && p->selected == p->child_item) {
		struct cow_menu_item *it = item_at_index(p->def, p->selected);
		if (it != NULL && it->is_popup)
			return;
	}
	cow_menu_popup_close(p->child);
}

static void
dispatch_action_with_context(const char *action, struct cow_view *view,
    bool pick_if_no_context, struct cow_seat *menu_seat)
{
	if (action == NULL)
		return;
	char *picked_action = NULL;
	if ((view == NULL || view->closed) && pick_if_no_context) {
		xasprintf(&picked_action, "pick-window { %s }", action);
		action = picked_action;
	}

	struct cmd_list *cmdlist = NULL;
	char		*cause	 = NULL;
	if (cmd_string_parse(action, &cmdlist, NULL, 0, &cause) != 0) {
		if (cause != NULL) {
			fprintf(stderr, "cow: menu command error: %s\n", cause);
			xfree(cause);
		}
		xfree(picked_action);
		return;
	}
	if (cmdlist == NULL) {
		xfree(picked_action);
		return;
	}

	struct cmd_q *cmdq = cmdq_new();
	cmdq->client_fd	   = -1;
	if (view != NULL && !view->closed)
		cmdq_set_default_target(cmdq, view, view->desk, view->output);
	struct cow_seat *old_seat = cow_wm.current_interaction_seat;
	cow_wm.current_interaction_seat =
	    (menu_seat != NULL && !menu_seat->removed) ? menu_seat : NULL;
	cmdq_run(cmdq, cmdlist);
	cow_wm.current_interaction_seat = old_seat;
	cmd_list_free(cmdlist);
	cmdq_free(cmdq);
	xfree(picked_action);
}

static struct cow_seat *
seat_by_name(uint32_t name)
{
	struct cow_seat *seat;
	wl_list_for_each(seat, &cow_wm.seats, wm_link)
	{
		if (!seat->removed && seat->wl_seat_name == name)
			return (seat);
	}
	return (NULL);
}

void
cow_menu_run_deferred_action(void)
{
	if (!g_deferred_action_ready || g_deferred_action == NULL)
		return;

	char *action = g_deferred_action;
	char *context_id = g_deferred_context_id;
	bool pick_context = g_deferred_pick_context;
	uint32_t seat_name = g_deferred_seat_name;
	g_deferred_action = NULL;
	g_deferred_context_id = NULL;
	g_deferred_action_ready = false;
	g_deferred_pick_context = false;
	g_deferred_seat_name = 0;

	struct cow_view *context = context_id != NULL ?
	    cow_view_find_by_identifier(context_id) : NULL;
	dispatch_action_with_context(action, context, pick_context,
	    seat_by_name(seat_name));
	xfree(context_id);
	xfree(action);
}

static void
dispatch_item(struct cow_menu_popup *p, struct cow_menu_item *it)
{
	if (it == NULL || it->action == NULL)
		return;
	if (it->is_popup) {
		struct cow_menu *sub = cow_menu_find(it->action);
		if (sub == NULL)
			return;
		const struct cow_menu_style *st	 = &p->style;
		struct cow_menu_item	    *iter;
		int			     y = p->y + st->border_width;
		int			     opener_idx = 0;
		wl_list_for_each(iter, &p->def->items, link)
		{
			if (iter == it)
				break;
			if (menu_item_visible(p->def, iter))
				y += item_row_height(iter,
				    p->item_h - st->padding_y * 2, st);
			opener_idx++;
		}
		int x = p->x + p->width - 5;
		struct cow_menu_popup *child =
		    cow_menu_popup_open(sub, x, y, cow_wm.menu_context_view, p,
			false);
		if (child != NULL) {
			p->child_item = opener_idx;
			child->pick_target_on_action =
			    p->pick_target_on_action || it->popup_pick_context;
		}
		return;
	}

	xfree(g_deferred_action);
	xfree(g_deferred_context_id);
	g_deferred_action = xstrdup(it->action);
	g_deferred_context_id = cow_wm.menu_context_view != NULL &&
	    cow_wm.menu_context_view->identifier != NULL ?
	    xstrdup(cow_wm.menu_context_view->identifier) : NULL;
	g_deferred_pick_context = p->pick_target_on_action;
	g_deferred_seat_name = cow_wm.menu_context_seat != NULL ?
	    cow_wm.menu_context_seat->wl_seat_name : 0;
	g_deferred_action_ready = false;
	cow_menu_close_all();
}

bool
cow_menu_process_input(void)
{
	if (!cow_menu_is_open())
		return (false);

	struct cow_menu_popup *root = wl_container_of(cow_wm.open_menus.next,
	    root, link);
	if (!g_menu_focus_requested && root->shell_surface != NULL &&
	    cow_wm.menu_context_seat != NULL) {
		if (g_menu_keyboard == NULL && cow_wm.wl_seat_raw != NULL &&
		    cow_wm.wl_seat_has_keyboard) {
			g_menu_keyboard = wl_seat_get_keyboard(cow_wm.wl_seat_raw);
			if (g_menu_keyboard != NULL)
				wl_keyboard_add_listener(g_menu_keyboard,
				    &menu_keyboard_listener, NULL);
		}
		river_seat_v1_focus_shell_surface(
		    cow_wm.menu_context_seat->river_seat, root->shell_surface);
		g_menu_focus_requested = true;
	}

	int		 px_x	 = 0;
	int		 px_y	 = 0;
	bool		 have	 = false;
	struct cow_seat *primary = cow_seat_primary();
	if (primary != NULL && primary->has_pointer_position) {
		px_x = primary->pointer_x;
		px_y = primary->pointer_y;
		have = true;
	} else if (cow_wm.has_raw_pointer) {
		px_x = cow_wm.raw_pointer_x;
		px_y = cow_wm.raw_pointer_y;
		have = true;
	}

	/* Static pointer position must not overwrite keyboard selection. */
	static int last_px_x = INT_MIN;
	static int last_px_y = INT_MIN;
	bool pointer_moved   = have && (last_px_x != px_x || last_px_y != px_y);
	if (have) {
		last_px_x = px_x;
		last_px_y = px_y;
	}
	if (pointer_moved)
		g_hover_suppressed_until_pointer_moves = false;

	struct cow_menu_popup *over = have ? popup_under(px_x, px_y) : NULL;

	if (pointer_moved && over != NULL) {
		struct cow_menu_popup *popups[POPUP_EXT_MAX];
		int		       n = 0;
		struct cow_menu_popup *p;
		wl_list_for_each(p, &cow_wm.open_menus, link)
		{
			if (n < (int)nitems(popups))
				popups[n++] = p;
		}

		for (int i = 0; i < n; i++) {
			p = popups[i];
			int want = (p == over) ? item_under(p, px_x, px_y) :
						 -1;
			if (p->child != NULL &&
			    popup_is_ancestor_of(p->child, over))
				want = p->selected;
			if (want != p->selected) {
				p->selected = want;
				p->needs_redraw		  = true;
				cow_wm.needs_manage_dirty = true;
			}
		}

		for (int i = 0; i < n; i++)
			popup_close_child_if_unrelated(popups[i], over);
	}

	if (!g_hover_suppressed_until_pointer_moves && over != NULL &&
	    over->selected >= 0) {
		struct cow_menu_item *it  = item_at_index(over->def,
		     over->selected);
		struct popup_ext     *e	  = ext_for(over, true);
		long		      now = mono_ms();
		if (it != NULL && it->is_popup) {
			if (e->hover_item != over->selected) {
				e->hover_item	    = over->selected;
				e->hover_started_ms = now;
			} else if (now - e->hover_started_ms >=
				MENU_POPUP_DELAY_MS &&
			    over->child == NULL) {
				dispatch_item(over, it);
			}
			cow_wm.needs_manage_dirty = true;
		} else if (e != NULL) {
			e->hover_item	    = -1;
			e->hover_started_ms = 0;
		}
	}

	struct cow_seat *seat = cow_seat_primary();
	if (seat != NULL && seat->menu_click_pending) {
		seat->menu_click_pending = false;
		if (over == NULL) {
			cow_menu_close_all();
			return (true);
		}
		int idx = item_under(over, px_x, px_y);
		if (idx < 0) {
			cow_menu_close_all();
			return (true);
		}
		struct cow_menu_item *it = item_at_index(over->def, idx);
		if (item_selectable(it) || (it != NULL && it->is_popup))
			dispatch_item(over, it);
	}

	return (true);
}

static struct cow_menu_popup *
deepest_open(void)
{
	struct cow_menu_popup *p, *deepest = NULL;
	wl_list_for_each(p, &cow_wm.open_menus, link) deepest = p;
	return (deepest);
}

static void
nav_move(struct cow_menu_popup *p, int step)
{
	int n = wl_list_length(&p->def->items);
	if (n == 0)
		return;
	int idx = p->selected;
	for (int tries = 0; tries < n; tries++) {
		idx += step;
		if (idx < 0)
			idx = n - 1;
		else if (idx >= n)
			idx = 0;
		struct cow_menu_item *it = item_at_index(p->def, idx);
		if (menu_item_visible(p->def, it) &&
		    (item_selectable(it) || (it != NULL && it->is_popup))) {
			p->selected = idx;
			cow_menu_popup_redraw(p);
			return;
		}
	}
}

static bool
nav_hotkey(struct cow_menu_popup *p, int key)
{
	if (p == NULL || key < 0)
		return (false);
	int		      idx = 0;
	struct cow_menu_item *it;
	wl_list_for_each(it, &p->def->items, link)
	{
		if (menu_item_visible(p->def, it) &&
		    (item_selectable(it) || (it != NULL && it->is_popup)) &&
		    menu_auto_hotkey_for(p->def, it) == key) {
			p->selected = idx;
			cow_menu_popup_redraw(p);
			dispatch_item(p, it);
			return (true);
		}
		idx++;
	}
	return (false);
}

static bool
label_contains_folded(const char *label, const char *query)
{
	if (label == NULL || query == NULL)
		return (false);
	if (*query == '\0')
		return (true);
	for (const char *start = label; *start != '\0'; start++) {
		const char *s = start;
		const char *q = query;
		while (*s != '\0' && *q != '\0') {
			if (*s == '&' && s[1] != '\0')
				s++;
			if (hotkey_fold((unsigned char)*s) !=
			    hotkey_fold((unsigned char)*q))
				break;
			s++;
			q++;
		}
		if (*q == '\0')
			return (true);
	}
	return (false);
}

static void
nav_typeahead_match(struct cow_menu_popup *p)
{
	int idx = 0;
	struct cow_menu_item *it;
	wl_list_for_each(it, &p->def->items, link)
	{
		if (item_selectable(it) && menu_item_visible(p->def, it)) {
			p->selected = idx;
			p->needs_layout = true;
			cow_menu_popup_redraw(p);
			return;
		}
		idx++;
	}
	p->selected = -1;
	p->needs_layout = true;
	cow_menu_popup_redraw(p);
}

static bool
nav_number(struct cow_menu_popup *p, int key)
{
	int wanted = key == '0' ? 10 : key - '0';
	int number = 0;
	int idx = 0;
	struct cow_menu_item *it;
	wl_list_for_each(it, &p->def->items, link)
	{
		if (menu_item_visible(p->def, it) && item_selectable(it) &&
		    label_explicit_hotkey(it->label) == key) {
			p->selected = idx;
			dispatch_item(p, it);
			return (true);
		}
		idx++;
	}

	idx = 0;
	wl_list_for_each(it, &p->def->items, link)
	{
		if (menu_item_visible(p->def, it) && item_selectable(it) &&
		    ++number == wanted) {
			p->selected = idx;
			dispatch_item(p, it);
			return (true);
		}
		idx++;
	}
	return (false);
}

static void
menu_navigate(const char *direction)
{
	if (!cow_menu_is_open() || direction == NULL)
		return;
	struct cow_menu_popup *p = deepest_open();
	if (p == NULL)
		return;

	g_hover_suppressed_until_pointer_moves = true;
	hover_reset_all();

	if (strcmp(direction, "up") == 0) {
		nav_move(p, -1);
	} else if (strcmp(direction, "down") == 0) {
		nav_move(p, 1);
	} else if (strcmp(direction, "right") == 0) {
		struct cow_menu_item *it = item_at_index(p->def, p->selected);
		if (it != NULL && it->is_popup)
			dispatch_item(p, it);
	} else if (strcmp(direction, "left") == 0) {
		if (p->parent != NULL)
			cow_menu_popup_close(p);
	} else if (strcmp(direction, "select") == 0) {
		struct cow_menu_item *it = item_at_index(p->def, p->selected);
		if (it != NULL)
			dispatch_item(p, it);
	} else if (strcmp(direction, "close") == 0) {
		cow_menu_close_all();
	} else if (p->def->typeahead && strcmp(direction, "backspace") == 0) {
		size_t len = strlen(p->def->typeahead_query);
		if (len > 0)
			p->def->typeahead_query[len - 1] = '\0';
		nav_typeahead_match(p);
	} else if (direction[0] != '\0' && direction[1] == '\0') {
		int key = hotkey_fold((unsigned char)direction[0]);
		if (p->def->numbered && isdigit((unsigned char)key)) {
			(void)nav_number(p, key);
		} else if (p->def->typeahead && isalnum((unsigned char)key)) {
			size_t len = strlen(p->def->typeahead_query);
			if (len + 1 < sizeof(p->def->typeahead_query)) {
				p->def->typeahead_query[len] = (char)key;
				p->def->typeahead_query[len + 1] = '\0';
			}
			nav_typeahead_match(p);
		} else {
			(void)nav_hotkey(p, key);
		}
	}
}
