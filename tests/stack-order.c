/* Check the ranks published by the same pass that orders River nodes. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include "cow/view.h"
#include "cow/wm.h"
#include "cow/stack.h"
#include "cow/update.h"
#include "river-window-management-v1-client-protocol.h"

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%d: %s\n", __LINE__, #expr); \
		abort(); \
	} \
} while (0)

int
main(void)
{
	int sockets[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	struct wl_display *display = wl_display_connect_to_fd(sockets[0]);
	CHECK(display != NULL);
	struct cow_view views[4] = {
		{ .layer = 0, .raise_seq = UINT64_MAX },
		{ .layer = 4, .raise_seq = UINT64_MAX - 1 },
		{ .layer = 4, .raise_seq = UINT64_MAX },
		{ .layer = 9, .flags = COW_VIEW_ICONIFIED_FLAG }
	};
	wl_list_init(&cow_wm.views);
	for (int i = 0; i < 4; i++) {
		views[i].node = (struct river_node_v1 *)wl_proxy_create(
		    (struct wl_proxy *)display, &river_node_v1_interface);
		CHECK(views[i].node != NULL);
		wl_list_insert(&cow_wm.views, &views[i].wm_link);
	}
	views[3].icon_node = views[3].node;
	cow_stack_mark_dirty();
	cow_update_reset();
	cow_stack_apply();
	CHECK(views[3].stack_order == 1 && views[0].stack_order == 2);
	CHECK(views[1].stack_order == 3 && views[2].stack_order == 4);
	CHECK(cow_update_take() & COW_UPDATE_STATUS);
	cow_stack_mark_dirty();
	cow_stack_apply();
	CHECK(cow_update_take() == COW_UPDATE_NONE);
	/* A child inherits the ancestor's effective stacking level. */
	views[0].parent = &views[2];
	struct cow_container_member member = { .view = &views[2] };
	struct cow_container container = { .active = &member };
	member.container = &container;
	views[2].container_member = &member;
	cow_stack_mark_dirty();
	cow_stack_apply();
	CHECK(views[0].stack_order > views[2].stack_order);
	for (int i = 0; i < 4; i++)
		wl_proxy_destroy((struct wl_proxy *)views[i].node);
	wl_display_disconnect(display);
	close(sockets[1]);
	return 0;
}
