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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xpm.h"

static int
check_pixel(const struct cow_xpm_image *image, int x, int y,
    const unsigned char expected[4])
{
	const unsigned char *actual = image->pixels +
	    ((size_t)y * image->width + x) * 4;
	if (memcmp(actual, expected, 4) == 0)
		return 0;
	fprintf(stderr, "pixel %d,%d: got %u,%u,%u,%u; expected %u,%u,%u,%u\n",
	    x, y, actual[0], actual[1], actual[2], actual[3], expected[0],
	    expected[1], expected[2], expected[3]);
	return 1;
}

int
main(int argc, char **argv)
{
	if (argc != 3)
		return 2;
	struct cow_xpm_image image;
	char *cause = NULL;
	if (!cow_xpm_load(argv[1], &image, &cause)) {
		fprintf(stderr, "%s\n", cause);
		free(cause);
		return 1;
	}
	int failed = image.width != 3 || image.height != 2;
	const unsigned char transparent[] = { 0, 0, 0, 0 };
	const unsigned char red[] = { 255, 0, 0, 255 };
	const unsigned char green[] = { 0, 255, 0, 255 };
	const unsigned char gray[] = { 128, 128, 128, 255 };
	failed |= check_pixel(&image, 0, 0, transparent);
	failed |= check_pixel(&image, 1, 0, red);
	failed |= check_pixel(&image, 2, 0, green);
	failed |= check_pixel(&image, 0, 1, gray);
	cow_xpm_free(&image);

	if (cow_xpm_load(argv[2], &image, &cause)) {
		fprintf(stderr, "invalid XPM was accepted\n");
		cow_xpm_free(&image);
		return 1;
	}
	if (cause == NULL || strstr(cause, "unknown colour key") == NULL) {
		fprintf(stderr, "unexpected error: %s\n",
		    cause != NULL ? cause : "none");
		failed = 1;
	}
	free(cause);
	return failed != 0;
}
