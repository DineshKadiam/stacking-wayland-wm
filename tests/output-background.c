/* Exercise background invalidation without a compositor or image decoder. */
#undef NDEBUG
#include <assert.h>
#include <wayland-client.h>
#include "cow/image.h"
#include "cow/shm.h"

static int decodes, commits;
static void
render_image(const char *path, struct cow_pixbuf *pixels)
{
	(void)path;
	(void)pixels;
	decodes++;
}

#define cow_image_render_tiled render_image
#define wl_surface_attach(...) ((void)0)
#define wl_surface_damage_buffer(...) ((void)0)
#define wl_surface_commit(...) ((void)commits++)
#include "../src/output.c"

int
main(void)
{
	cow_config = cow_config_create();
	uint32_t pixels[16] = { 0 };
	struct cow_shm_buffer buffer = {
		.width = 4, .height = 4, .stride = 16, .data = pixels,
	};
	struct cow_output output = {
		.width = 4, .height = 4, .bg_configured = true,
		.bg_buffer = &buffer, .has_bg_image = true,
		.bg_image = xstrdup("wallpaper.png"),
	};
	output.bg_surface = (void *)&output;
	output.bg_layer_surface = (void *)&output;
	cow_output_render_background(&output);
	assert(decodes == 1 && commits == 1);
	/* An unchanged busy buffer stays attached; no allocation or decode. */
	for (int i = 0; i < 20; i++)
		cow_output_render_background(&output);
	output_handle_dimensions(&output, NULL, 4, 4);
	assert(output.bg_buffer == &buffer && !output.bg_dirty);
	cow_output_render_background(&output);
	assert(decodes == 1 && commits == 1);

	/* Reapplying output policy with unchanged effective inputs is cheap. */
	xstrset(&cow_config->output_bg_image, "wallpaper.png");
	cow_config->has_output_bg_image = true;
	output_apply_configured_background(&output);
	cow_output_render_background(&output);
	assert(decodes == 1 && commits == 1);

	/* Actual colour/image changes and explicit reloads must still render. */
	buffer.busy = false;
	cow_config->has_output_bg_colour = true;
	cow_config->output_bg_colour[3] = 1;
	cow_output_render_background(&output);
	assert(decodes == 2 && commits == 2);
	buffer.busy = false;
	xstrset(&cow_config->output_bg_image, "replacement.png");
	cow_output_render_background(&output);
	assert(decodes == 3 && commits == 3);
	buffer.busy = false;
	output.bg_dirty = true;
	cow_output_render_background(&output);
	assert(decodes == 4 && commits == 4);
	buffer.busy = false;
	xstrset(&cow_config->output_bg_image, NULL);
	cow_output_render_background(&output);
	assert(decodes == 4 && commits == 5);

	xfree(output.bg_image);
	xfree(output.bg_rendered_image);
	cow_config_destroy(cow_config);
	return 0;
}
