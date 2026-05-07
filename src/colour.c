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
#include "cow/colour.h"

#include <stdlib.h>

bool
cow_colour_parse(const char *str, float out[static 4])
{
	char	     *end;
	unsigned long v = strtoul(str, &end, 0);
	if (end == str)
		return false;
	cow_colour_convert(out, (uint32_t)v);
	return true;
}

uint32_t
cow_colour_to_pixel(const float colour[static 4])
{
	uint8_t r = (uint8_t)(colour[0] * 255.0f + 0.5f);
	uint8_t g = (uint8_t)(colour[1] * 255.0f + 0.5f);
	uint8_t b = (uint8_t)(colour[2] * 255.0f + 0.5f);
	uint8_t a = (uint8_t)(colour[3] * 255.0f + 0.5f);
	return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) |
	    b;
}

void
cow_colour_convert(float dst[static 4], uint32_t colour)
{
	dst[0] = ((colour >> 16) & 0xff) / 255.0;
	dst[1] = ((colour >> 8) & 0xff) / 255.0;
	dst[2] = (colour & 0xff) / 255.0;
	dst[3] = 1.0;
}

/*
 * HSL-based colour scaling from fvwm3/LessTif ColorUtils.c.
 *
 * Classifies colors by perceptual brightness into dark/medium/light,
 * then applies Motif-style highlight/shadow adjustments.
 *
 * Works in 16-bit integer space (SCALE = 65535) to match the X11
 * Motif implementation exactly.  cow_colour_mult() multiplies both
 * luminance and saturation by factor k, preserving hue.
 */
static inline void
cow_colour_mult(float *r, float *g, float *b, float k)
{
#define SCALE 65535.0

	unsigned int ri = (unsigned int)(*r * SCALE + 0.5);
	unsigned int gi = (unsigned int)(*g * SCALE + 0.5);
	unsigned int bi = (unsigned int)(*b * SCALE + 0.5);

	if (ri == gi && ri == bi) {
		/* Grey: simple multiply */
		double temp = k * (double)ri;
		if (temp > SCALE)
			temp = SCALE;
		*r = (float)(temp / SCALE);
		*g = *r;
		*b = *r;
		return;
	}

	unsigned int max_v, min_v;
	double	     a;
	int	     state;

	if (ri > gi) {
		if (ri > bi) {
			max_v = ri;
			if (gi < bi) {
				min_v = gi;
				state = 0;
				a     = (double)(bi - gi);
			} else {
				min_v = bi;
				state = 1;
				a     = (double)(gi - bi);
			}
		} else {
			max_v = bi;
			min_v = gi;
			state = 5;
			a     = (double)(ri - gi);
		}
	} else {
		if (gi > bi) {
			max_v = gi;
			if (bi < ri) {
				min_v = bi;
				state = 2;
				a     = (double)(ri - bi);
			} else {
				min_v = ri;
				state = 3;
				a     = (double)(bi - ri);
			}
		} else {
			max_v = bi;
			min_v = ri;
			state = 4;
			a     = (double)(gi - ri);
		}
	}

	double delta = (double)(max_v - min_v);
	a	     = a / delta;

	double l = ((double)max_v + (double)min_v) / 2.0;
	double s;
	if (l <= SCALE / 2.0) {
		s = delta / ((double)max_v + (double)min_v);
	} else {
		s = delta / (2.0 * SCALE - (double)max_v - (double)min_v);
	}

	l *= k;
	if (l > SCALE)
		l = SCALE;
	s *= k;
	if (s > 1.0)
		s = 1.0;

	double new_max, new_min, middle;
	if (l <= SCALE / 2.0) {
		new_max = l * (1.0 + s);
	} else {
		new_max = s * SCALE + l - s * l;
	}
	new_min = 2.0 * l - new_max;
	middle	= new_min + (new_max - new_min) * a;

	/* Clamp */
	if (new_max > SCALE)
		new_max = SCALE;
	if (new_min < 0)
		new_min = 0;
	if (middle > SCALE)
		middle = SCALE;
	if (middle < 0)
		middle = 0;

	switch (state) {
	case 0:
		ri = (unsigned int)new_max;
		gi = (unsigned int)new_min;
		bi = (unsigned int)middle;
		break;
	case 1:
		ri = (unsigned int)new_max;
		gi = (unsigned int)middle;
		bi = (unsigned int)new_min;
		break;
	case 2:
		ri = (unsigned int)middle;
		gi = (unsigned int)new_max;
		bi = (unsigned int)new_min;
		break;
	case 3:
		ri = (unsigned int)new_min;
		gi = (unsigned int)new_max;
		bi = (unsigned int)middle;
		break;
	case 4:
		ri = (unsigned int)new_min;
		gi = (unsigned int)middle;
		bi = (unsigned int)new_max;
		break;
	case 5:
		ri = (unsigned int)middle;
		gi = (unsigned int)new_min;
		bi = (unsigned int)new_max;
		break;
	default:
		/* unreachable: state is computed into [0,5] above */
		break;
	}

	*r = (float)ri / (float)SCALE;
	*g = (float)gi / (float)SCALE;
	*b = (float)bi / (float)SCALE;
#undef SCALE
}

void
cow_colour_lighten(float dst[static 4], const float src[static 4])
{
	unsigned int r		= (unsigned int)(src[0] * 65535.0f + 0.5f);
	unsigned int g		= (unsigned int)(src[1] * 65535.0f + 0.5f);
	unsigned int b		= (unsigned int)(src[2] * 65535.0f + 0.5f);
	unsigned int brightness = 2 * r + 3 * g + 1 * b;

	unsigned int dark_thr  = (unsigned int)(6.0 * 65535.0 * 0.15);
	unsigned int light_thr = (unsigned int)(6.0 * 65535.0 * 0.85);

	if (brightness < dark_thr) {
		r = 65535 - ((65535 - r) * 50 + 50) / 100;
		g = 65535 - ((65535 - g) * 50 + 50) / 100;
		b = 65535 - ((65535 - b) * 50 + 50) / 100;
	} else if (brightness > light_thr) {
		r = (r * 80 + 50) / 100;
		g = (g * 80 + 50) / 100;
		b = (b * 80 + 50) / 100;
	} else {
		dst[0] = src[0];
		dst[1] = src[1];
		dst[2] = src[2];
		cow_colour_mult(&dst[0], &dst[1], &dst[2], 1.4f);
		dst[3] = src[3];
		return;
	}
	dst[0] = (float)r / 65535.0f;
	dst[1] = (float)g / 65535.0f;
	dst[2] = (float)b / 65535.0f;
	dst[3] = src[3];
}

void
cow_colour_darken(float dst[static 4], const float src[static 4])
{
	unsigned int r		= (unsigned int)(src[0] * 65535.0f + 0.5f);
	unsigned int g		= (unsigned int)(src[1] * 65535.0f + 0.5f);
	unsigned int b		= (unsigned int)(src[2] * 65535.0f + 0.5f);
	unsigned int brightness = 2 * r + 3 * g + 1 * b;

	unsigned int dark_thr  = (unsigned int)(6.0 * 65535.0 * 0.15);
	unsigned int light_thr = (unsigned int)(6.0 * 65535.0 * 0.85);

	if (brightness < dark_thr) {
		r = 65535 - ((65535 - r) * 70 + 50) / 100;
		g = 65535 - ((65535 - g) * 70 + 50) / 100;
		b = 65535 - ((65535 - b) * 70 + 50) / 100;
	} else if (brightness > light_thr) {
		r = (r * 55 + 50) / 100;
		g = (g * 55 + 50) / 100;
		b = (b * 55 + 50) / 100;
	} else {
		dst[0] = src[0];
		dst[1] = src[1];
		dst[2] = src[2];
		cow_colour_mult(&dst[0], &dst[1], &dst[2], 0.5f);
		dst[3] = src[3];
		return;
	}
	dst[0] = (float)r / 65535.0f;
	dst[1] = (float)g / 65535.0f;
	dst[2] = (float)b / 65535.0f;
	dst[3] = src[3];
}

void
cow_colour_make_triple(struct cow_colour_triple *out,
    const float					 colour[static 4])
{
	float lt[4], dk[4];
	cow_colour_lighten(lt, colour);
	cow_colour_darken(dk, colour);
	out->base = cow_colour_to_pixel(colour);
	out->lt	  = cow_colour_to_pixel(lt);
	out->dk	  = cow_colour_to_pixel(dk);
}

const float COW_COLOUR_DROP_ORANGE[4] = { 1.0f, 0.53f, 0.0f, 1.0f };
const float COW_COLOUR_WHITE[4]	      = { 1.0f, 1.0f, 1.0f, 1.0f };
