/* Timed and explicitly dismissed text, rendered as passive menus. */
#include <limits.h>
#include <string.h>

#include "cow/notification.h"
#include "cow/memory.h"
#include "cow/menu.h"
#include "cow/output.h"
#include "cow/update.h"
#include "cow/util.h"
#include "cow/view.h"
#include "cow/wm.h"

struct cow_notification {
	struct wl_list link;
	struct cow_menu menu;
	struct cow_menu_popup *popup;
	struct cow_notification **owner;
	char *name, *view_id, *output_name;
	long expires;
};

static struct wl_list notifications = {&notifications, &notifications};

void
cow_notification_hide(struct cow_notification *notice)
{
	if (notice == NULL)
		return;
	if (notice->owner != NULL)
		*notice->owner = NULL;
	wl_list_remove(&notice->link);
	cow_menu_popup_close(notice->popup);
	cow_menu_clear(&notice->menu);
	xfree(notice->name);
	xfree(notice->view_id);
	xfree(notice->output_name);
	xfree(notice);
}

bool
cow_notification_dismiss(const char *name)
{
	struct cow_notification *notice;
	wl_list_for_each(notice, &notifications, link) {
		if (notice->name != NULL && strcmp(notice->name, name) == 0) {
			cow_notification_hide(notice);
			return true;
		}
	}
	return false;
}

void
cow_notification_show(const char *name, struct cow_notification **owner,
    const char *text, struct cow_view *view, struct cow_output *output,
    int timeout)
{
	if (name != NULL)
		cow_notification_dismiss(name);
	if (owner != NULL)
		cow_notification_hide(*owner);
	struct cow_notification *notice = xcalloc(1, sizeof(*notice));
	notice->owner = owner;
	if (owner != NULL)
		*owner = notice;
	if (name != NULL)
		notice->name = xstrdup(name);
	if (view != NULL && view->identifier != NULL)
		notice->view_id = xstrdup(view->identifier);
	if (output != NULL && output->name != NULL)
		notice->output_name = xstrdup(output->name);
	notice->expires = timeout > 0 ? cow_mono_ms() + timeout : 0;
	wl_list_init(&notice->menu.items);
	notice->menu.literal_labels = true;
	char *copy = xstrdup(text);
	char *cursor = copy;
	char *line;
	bool title = true;
	while ((line = strsep(&cursor, "\n")) != NULL) {
		cow_menu_item_add(&notice->menu, title ? COW_MENU_ITEM_TITLE :
		    COW_MENU_ITEM_DISABLED, line, NULL, false, false, NULL);
		title = false;
	}
	xfree(copy);
	notice->popup = cow_menu_popup_open_passive(&notice->menu);
	wl_list_insert(notifications.prev, &notice->link);
	cow_update_request(COW_UPDATE_MANAGE);
}

void
cow_notification_clear(void)
{
	struct cow_notification *notice, *tmp;
	wl_list_for_each_safe(notice, tmp, &notifications, link)
		cow_notification_hide(notice);
}

int
cow_notification_next_ms(void)
{
	int wait = -1;
	long now = cow_mono_ms();
	struct cow_notification *notice;
	wl_list_for_each(notice, &notifications, link) {
		if (notice->expires == 0)
			continue;
		long remaining = notice->expires - now;
		int ms = remaining <= 0 ? 0 :
		    remaining > INT_MAX ? INT_MAX : (int)remaining;
		if (wait < 0 || ms < wait)
			wait = ms;
	}
	return wait;
}

void
cow_notification_render_all(void)
{
	long now = cow_mono_ms();
	struct cow_notification *notice, *tmp;
	wl_list_for_each_safe(notice, tmp, &notifications, link) {
		struct cow_view *view = notice->view_id != NULL ?
		    cow_view_find_by_identifier(notice->view_id) : NULL;
		if (cow_wm.session_locked ||
		    (notice->expires != 0 && now >= notice->expires) ||
		    (notice->view_id != NULL && view == NULL)) {
			cow_notification_hide(notice);
			continue;
		}
		struct cow_output *out = view != NULL ? view->output :
		    notice->output_name != NULL ?
		    cow_output_find_by_name(notice->output_name) : NULL;
		if (out == NULL)
			out = cow_wm.current_output;
		if (out == NULL)
			continue;
		int x = view != NULL ? view->x : out->x + out->width / 2;
		int y = view != NULL ? view->y : out->y + out->height / 2;
		cow_menu_popup_render_passive(notice->popup, out, x, y,
		    view == NULL);
	}
}
