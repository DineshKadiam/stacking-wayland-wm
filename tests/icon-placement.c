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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cow/config.h"
#include "cow/icon_placement.h"
#include "cow/output.h"

#define CHECK(expr)                                                          \
	do {                                                                   \
		if (!(expr)) {                                                  \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
			    __LINE__, #expr);                                    \
			abort();                                                   \
		}                                                              \
	} while (0)

static void
test_geometry(void)
{
	struct cow_icon_box_config cfg;
	CHECK(cow_icon_box_parse("100x70-1-2", &cfg));
	CHECK(!cow_icon_box_parse("100x70+1", &cfg));
	struct cow_box usable = { 10, 20, 500, 400 };
	struct cow_box box;
	CHECK(cow_icon_box_resolve(&cfg, &usable, &box));
	CHECK(box.x == 409 && box.y == 348);
	CHECK(box.width == 100 && box.height == 70);
	CHECK(cow_icon_box_parse("full", &cfg));
	CHECK(cow_icon_box_resolve(&cfg, &usable, &box));
	CHECK(memcmp(&box, &usable, sizeof(box)) == 0);
	struct cow_icon_placement placement;
	cow_icon_placement_default(&placement);
	CHECK(placement.compact);
	CHECK(cow_icon_box_resolve(&placement.box, &usable, &box));
	CHECK(box.x == 18 && box.y == 28);
	CHECK(box.width == 484 && box.height == 384);
	CHECK(cow_icon_box_parse("100x70-999999999-999999999", &cfg));
	CHECK(!cow_icon_box_resolve(&cfg, &usable, &box));
}

static void
test_fill_and_collision(void)
{
	const char *directions[] = { "right", "left", "down", "up" };
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			enum cow_icon_direction primary, secondary;
			bool valid = cow_icon_fill_parse(directions[i],
			    directions[j], &primary, &secondary);
			CHECK(valid == ((i < 2) != (j < 2)));
		}
	}
	struct cow_icon_placement p;
	cow_icon_placement_default(&p);
	p.box.type = COW_ICON_BOX_FULL;
	p.grid_x = 60;
	p.grid_y = 60;
	struct cow_box box = { 0, 0, 200, 200 };
	int x, y;
	CHECK(cow_icon_placement_find(&p, &box, 50, 50, NULL, 0, &x, &y));
	CHECK(x == 0 && y == 150);
	struct cow_icon_rect occupied = { x, y, 50, 50 };
	CHECK(cow_icon_placement_find(&p, &box, 50, 50, &occupied, 1,
	    &x, &y));
	CHECK(x == 60 && y == 150);

	CHECK(cow_icon_fill_parse("down", "left", &p.primary,
	    &p.secondary));
	CHECK(!cow_icon_fill_parse("right", "left", &p.primary,
	    &p.secondary));
	CHECK(cow_icon_fill_parse("down", "left", &p.primary,
	    &p.secondary));
	CHECK(cow_icon_placement_find(&p, &box, 50, 50, NULL, 0, &x, &y));
	CHECK(x == 150 && y == 0);
}

static void
test_output_override(void)
{
	struct cow_config config = { 0 };
	cow_icon_placement_config_init(&config);
	config.icon_placement.grid_x = 17;
	struct cow_icon_placement_override *o =
	    cow_icon_placement_override_get(&config, "DP-1", true);
	o->fields = COW_ICON_PLACEMENT_FILL;
	o->placement.primary = COW_ICON_DIR_DOWN;
	o->placement.secondary = COW_ICON_DIR_LEFT;
	struct cow_output output = { .name = "DP-1", .number = 1 };
	struct cow_icon_placement resolved;
	cow_icon_placement_resolve(&config, &output, &resolved);
	CHECK(resolved.grid_x == 17);
	CHECK(resolved.primary == COW_ICON_DIR_DOWN);
	CHECK(resolved.secondary == COW_ICON_DIR_LEFT);
	cow_icon_placement_override_remove(&config, "DP-1");
	cow_icon_placement_resolve(&config, &output, &resolved);
	CHECK(resolved.primary == COW_ICON_DIR_RIGHT);
	o = cow_icon_placement_override_get(&config, "@1", true);
	o->fields = COW_ICON_PLACEMENT_GRID;
	o->placement.grid_x = 19;
	o->placement.grid_y = 23;
	cow_icon_placement_resolve(&config, &output, &resolved);
	CHECK(resolved.grid_x == 19 && resolved.grid_y == 23);
	o->fields |= COW_ICON_PLACEMENT_COMPACT;
	o->placement.compact = false;
	cow_icon_placement_resolve(&config, &output, &resolved);
	CHECK(!resolved.compact);
	cow_icon_placement_config_fini(&config);
}

int
main(void)
{
	test_geometry();
	test_fill_and_collision();
	test_output_override();
	return 0;
}
