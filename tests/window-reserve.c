#undef NDEBUG
#include <assert.h>
#include "cow/config.h"
#include "cow/container.h"
#include "cow/output.h"
#include "cow/view.h"
#include "cow/wm.h"

int
main(void)
{
	wl_list_init(&cow_wm.views);
	wl_list_init(&cow_wm.outputs);
	cow_config = cow_config_create();
	cow_config->reserved.bottom = 10;
	struct cow_output output = {
	    .width = 800, .height = 600,
	    .has_layer_usable_area = true,
	    .layer_usable_area = {0, 20, 800, 580},
	};
	struct cow_output other = {.x = 800, .width = 800, .height = 600};
	struct cow_view panel = {.output = &output, .reserve_edge = COW_RESERVE_BOTTOM,
	    .content_width = 800, .content_height = 96, .has_dimensions = true};
	struct cow_view second = {.output = &output, .reserve_edge = COW_RESERVE_BOTTOM,
	    .content_width = 300, .content_height = 48, .has_dimensions = true};
	wl_list_insert(&cow_wm.views, &panel.wm_link);
	wl_list_insert(&cow_wm.views, &second.wm_link);
	assert(cow_output_recompute_usable_area(&output));
	assert(output.usable_area.y == 20);
	assert(output.usable_area.height == 474);
	assert(cow_output_base_usable_area(&output).height == 570);
	assert(!cow_output_recompute_usable_area(&output));
	cow_output_recompute_usable_area(&other);
	assert(other.usable_area.height == 590);
	panel.reserve_edge = COW_RESERVE_NONE; /* Keep the other owner. */
	assert(cow_output_recompute_usable_area(&output));
	assert(output.usable_area.height == 522);
	/* A swallowed client cannot reserve desktop space independently. */
	struct cow_container_pane pane = {.layout.axis = COW_CONTAINER_LEAF};
	struct cow_container attached = {.attached = true, .root = &pane};
	struct cow_container_member member = {.container = &attached};
	second.container_member = &member;
	assert(cow_output_recompute_usable_area(&output));
	assert(output.usable_area.height == 570);
	second.container_member = NULL;
	second.content_height = 72;
	cow_output_recompute_usable_area(&output);
	assert(output.usable_area.height == 498);
	second.closed = true;
	cow_output_recompute_usable_area(&output);
	assert(output.usable_area.height == 570);
	second.closed = false;
	second.output = &other;
	cow_output_recompute_usable_area(&output);
	cow_output_recompute_usable_area(&other);
	assert(output.usable_area.height == 570);
	assert(other.usable_area.height == 518);
	second.flags |= COW_VIEW_ICONIFIED_FLAG;
	cow_output_recompute_usable_area(&other);
	assert(other.usable_area.height == 590);
	wl_list_remove(&panel.wm_link);
	wl_list_remove(&second.wm_link);
	cow_config_destroy(cow_config);
	return 0;
}
