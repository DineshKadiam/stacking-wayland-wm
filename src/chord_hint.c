/* Chord hints use private notifications, separate from user-named notices. */
#include "cow/chord_hint.h"
#include "cow/config.h"
#include "cow/memory.h"
#include "cow/notification.h"
#include "cow/output.h"
#include "cow/seat.h"
#include "cow/view.h"
#include "cow/wm.h"

void
cow_chord_hint_hide(struct cow_seat *seat)
{
	cow_notification_hide(seat->chord_hint);
}

void
cow_chord_hint_show(struct cow_seat *seat, const char *text)
{
	cow_chord_hint_hide(seat);
	if (cow_config == NULL || !cow_config->binding_sequence_hints)
		return;
	struct cow_view *view = seat->binding_sequence_view;
	char *label;
	if (view != NULL && view->title != NULL)
		xasprintf(&label, "%s\n%s", view->title, text);
	else
		label = xstrdup(text);
	cow_notification_show(NULL, &seat->chord_hint, label, view,
	    cow_output_at(seat->pointer_x, seat->pointer_y), 0);
	xfree(label);
}

void
cow_chord_hint_update(void)
{
	if (cow_config != NULL && cow_config->binding_sequence_hints)
		return;
	struct cow_seat *seat;
	wl_list_for_each(seat, &cow_wm.seats, wm_link)
		cow_chord_hint_hide(seat);
}
