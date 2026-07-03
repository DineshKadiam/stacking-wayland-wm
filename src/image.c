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
#include "cow/image.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cow/memory.h"

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

enum cow_image_type {
	COW_IMAGE_NONE,
	COW_IMAGE_PNG,
	COW_IMAGE_SVG,
};

static enum cow_image_type
image_type(const char *path)
{
	const char *dot = strrchr(path, '.');

	if (dot == NULL)
		return COW_IMAGE_NONE;
	if (strcasecmp(dot, ".png") == 0)
		return COW_IMAGE_PNG;
	if (strcasecmp(dot, ".svg") == 0)
		return COW_IMAGE_SVG;
	return COW_IMAGE_NONE;
}

static void
blend_rgba(struct cow_pixbuf *dst, int x, int y, const unsigned char *src)
{
	if (x < 0 || y < 0 || x >= dst->width || y >= dst->height)
		return;

	unsigned int sa = src[3];
	if (sa == 0)
		return;

	uint32_t *dp = &dst->pixels[y * dst->stride_px + x];
	uint32_t  d = *dp;
	unsigned int inv = 255 - sa;
	unsigned int da = (d >> 24) & 0xff;
	unsigned int dr = (d >> 16) & 0xff;
	unsigned int dg = (d >> 8) & 0xff;
	unsigned int db = d & 0xff;
	unsigned int r = (src[0] * sa + dr * inv) / 255;
	unsigned int g = (src[1] * sa + dg * inv) / 255;
	unsigned int b = (src[2] * sa + db * inv) / 255;
	unsigned int a = sa + da * inv / 255;

	*dp = (a << 24) | (r << 16) | (g << 8) | b;
}

bool
cow_image_validate(const char *path, char **cause)
{
	int w, h, comp;

	switch (image_type(path)) {
	case COW_IMAGE_PNG:
		if (stbi_info(path, &w, &h, &comp))
			return true;
		xasprintf(cause, "%s: not a readable PNG", path);
		return false;
	case COW_IMAGE_SVG: {
		NSVGimage *svg = nsvgParseFromFile(path, "px", 96.0f);
		if (svg != NULL) {
			nsvgDelete(svg);
			return true;
		}
		xasprintf(cause, "%s: not a readable SVG", path);
		return false;
	}
	default:
		xasprintf(cause, "%s: expected .png or .svg", path);
		return false;
	}
}

static void
render_rgba_scaled(unsigned char *src, int iw, int ih, struct cow_pixbuf *dst,
    int x, int y, int w, int h)
{
	for (int dy = 0; dy < h; dy++) {
		float syf = ((dy + 0.5f) * ih / h) - 0.5f;
		int   sy0 = (int)floorf(syf);
		float fy = syf - sy0;
		if (sy0 < 0) {
			sy0 = 0;
			fy = 0.0f;
		}
		int sy1 = sy0 + 1;
		if (sy1 >= ih)
			sy1 = ih - 1;
		for (int dx = 0; dx < w; dx++) {
			float sxf = ((dx + 0.5f) * iw / w) - 0.5f;
			int   sx0 = (int)floorf(sxf);
			float fx = sxf - sx0;
			if (sx0 < 0) {
				sx0 = 0;
				fx = 0.0f;
			}
			int sx1 = sx0 + 1;
			if (sx1 >= iw)
				sx1 = iw - 1;

			unsigned char rgba[4];
			const unsigned char *p00 = src + ((sy0 * iw + sx0) * 4);
			const unsigned char *p10 = src + ((sy0 * iw + sx1) * 4);
			const unsigned char *p01 = src + ((sy1 * iw + sx0) * 4);
			const unsigned char *p11 = src + ((sy1 * iw + sx1) * 4);
			for (int c = 0; c < 4; c++) {
				float top = p00[c] + (p10[c] - p00[c]) * fx;
				float bot = p01[c] + (p11[c] - p01[c]) * fx;
				float v = top + (bot - top) * fy;
				if (v < 0.0f)
					v = 0.0f;
				if (v > 255.0f)
					v = 255.0f;
				rgba[c] = (unsigned char)(v + 0.5f);
			}
			blend_rgba(dst, x + dx, y + dy, rgba);
		}
	}
}

static void
render_png(const char *path, struct cow_pixbuf *dst, int x, int y, int w,
    int h)
{
	int iw, ih, comp;
	unsigned char *src = stbi_load(path, &iw, &ih, &comp, 4);

	if (src == NULL)
		return;
	render_rgba_scaled(src, iw, ih, dst, x, y, w, h);
	stbi_image_free(src);
}

static void
render_svg(const char *path, struct cow_pixbuf *dst, int x, int y, int w,
    int h)
{
	NSVGimage *svg = nsvgParseFromFile(path, "px", 96.0f);

	if (svg == NULL)
		return;
	NSVGrasterizer *rast = nsvgCreateRasterizer();
	if (rast == NULL) {
		nsvgDelete(svg);
		return;
	}

	int rw = (int)ceilf(svg->width);
	int rh = (int)ceilf(svg->height);
	if (rw < 1)
		rw = 1;
	if (rh < 1)
		rh = 1;

	unsigned char *src = xcalloc((size_t)rw * rh, 4);
	nsvgRasterize(rast, svg, 0, 0, 1.0f, src, rw, rh, rw * 4);
	render_rgba_scaled(src, rw, rh, dst, x, y, w, h);

	xfree(src);
	nsvgDeleteRasterizer(rast);
	nsvgDelete(svg);
}

void
cow_image_render_rect(const char *path, struct cow_pixbuf *dst, int x, int y,
    int w, int h)
{
	if (path == NULL || w <= 0 || h <= 0)
		return;

	switch (image_type(path)) {
	case COW_IMAGE_PNG:
		render_png(path, dst, x, y, w, h);
		break;
	case COW_IMAGE_SVG:
		render_svg(path, dst, x, y, w, h);
		break;
	default:
		break;
	}
}
