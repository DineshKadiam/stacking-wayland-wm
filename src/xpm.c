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

/* Dependency-free XPM3 decoder. */

#include "xpm.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cow/memory.h"

#define XPM_MAX_FILE_SIZE (64U * 1024U * 1024U)
#define XPM_MAX_PIXELS (64U * 1024U * 1024U)
#define XPM_MAX_COLOURS 65536U
#define XPM_MAX_CPP 32U

struct xpm_strings {
	char  **items;
	size_t  count;
	size_t  capacity;
};

struct xpm_colour {
	char		 key[XPM_MAX_CPP + 1];
	unsigned char	 rgba[4];
};

struct named_colour {
	const char    *name;
	unsigned char r, g, b;
};

static const struct named_colour named_colours[] = {
	{ "black", 0x00, 0x00, 0x00 },
	{ "silver", 0xc0, 0xc0, 0xc0 },
	{ "gray", 0xbe, 0xbe, 0xbe },
	{ "grey", 0xbe, 0xbe, 0xbe },
	{ "white", 0xff, 0xff, 0xff },
	{ "maroon", 0x80, 0x00, 0x00 },
	{ "red", 0xff, 0x00, 0x00 },
	{ "purple", 0x80, 0x00, 0x80 },
	{ "fuchsia", 0xff, 0x00, 0xff },
	{ "magenta", 0xff, 0x00, 0xff },
	{ "green", 0x00, 0x80, 0x00 },
	{ "lime", 0x00, 0xff, 0x00 },
	{ "olive", 0x80, 0x80, 0x00 },
	{ "yellow", 0xff, 0xff, 0x00 },
	{ "navy", 0x00, 0x00, 0x80 },
	{ "blue", 0x00, 0x00, 0xff },
	{ "teal", 0x00, 0x80, 0x80 },
	{ "aqua", 0x00, 0xff, 0xff },
	{ "cyan", 0x00, 0xff, 0xff },
	{ "darkgray", 0xa9, 0xa9, 0xa9 },
	{ "darkgrey", 0xa9, 0xa9, 0xa9 },
	{ "lightgray", 0xd3, 0xd3, 0xd3 },
	{ "lightgrey", 0xd3, 0xd3, 0xd3 },
};

static void
xpm_error(char **cause, const char *fmt, ...)
{
	if (cause == NULL)
		return;
	va_list ap;
	va_start(ap, fmt);
	if (vasprintf(cause, fmt, ap) < 0)
		log_fatal("vasprintf: %s", strerror(errno));
	va_end(ap);
}

static void
strings_free(struct xpm_strings *strings)
{
	for (size_t i = 0; i < strings->count; i++)
		xfree(strings->items[i]);
	xfree(strings->items);
	memset(strings, 0, sizeof(*strings));
}

static void
strings_add(struct xpm_strings *strings, char *value)
{
	if (strings->count == strings->capacity) {
		strings->capacity = strings->capacity == 0 ? 32 :
		    strings->capacity * 2;
		strings->items = xrealloc(strings->items, strings->capacity,
		    sizeof(*strings->items));
	}
	strings->items[strings->count++] = value;
}

static void
string_append(char **value, size_t *length, size_t *capacity, char c)
{
	if (*length + 1 >= *capacity) {
		*capacity *= 2;
		*value = xrealloc(*value, *capacity, 1);
	}
	(*value)[(*length)++] = c;
}

static int
hex_digit(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static bool
extract_strings(const char *data, size_t len, struct xpm_strings *strings,
    char **cause)
{
	size_t i = 0;
	while (i < len) {
		if (i + 1 < len && data[i] == '/' && data[i + 1] == '*') {
			i += 2;
			while (i + 1 < len &&
			    !(data[i] == '*' && data[i + 1] == '/'))
				i++;
			if (i + 1 >= len) {
				xpm_error(cause, "unterminated comment");
				return false;
			}
			i += 2;
			continue;
		}
		if (i + 1 < len && data[i] == '/' && data[i + 1] == '/') {
			i += 2;
			while (i < len && data[i] != '\n')
				i++;
			continue;
		}
		if (data[i] != '"') {
			i++;
			continue;
		}

		i++;
		size_t capacity = 64;
		char *value = xmalloc(capacity);
		size_t out = 0;
		bool closed = false;
		while (i < len) {
			unsigned char c = (unsigned char)data[i++];
			if (c == '"') {
				closed = true;
				break;
			}
			if (c != '\\') {
				if (c == '\n' || c == '\r' || c == '\0')
					break;
				string_append(&value, &out, &capacity, (char)c);
				continue;
			}
			if (i >= len)
				break;
			c = (unsigned char)data[i++];
			switch (c) {
			case '\\': case '"': case '\'': case '?':
				string_append(&value, &out, &capacity, (char)c);
				break;
			case 'a': string_append(&value, &out, &capacity, '\a'); break;
			case 'b': string_append(&value, &out, &capacity, '\b'); break;
			case 'f': string_append(&value, &out, &capacity, '\f'); break;
			case 'n': string_append(&value, &out, &capacity, '\n'); break;
			case 'r': string_append(&value, &out, &capacity, '\r'); break;
			case 't': string_append(&value, &out, &capacity, '\t'); break;
			case 'v': string_append(&value, &out, &capacity, '\v'); break;
			case 'x': {
				unsigned int v = 0;
				int digit, count = 0;
				while (i < len &&
				    (digit = hex_digit((unsigned char)data[i])) >= 0) {
					v = (v << 4) | (unsigned int)digit;
					i++;
					count++;
				}
				if (count == 0 || v == 0 || v > 255) {
					xfree(value);
					xpm_error(cause, "invalid hexadecimal escape");
					return false;
				}
				string_append(&value, &out, &capacity, (char)v);
				break;
			}
			default:
				if (c < '0' || c > '7') {
					xfree(value);
					xpm_error(cause, "invalid escape sequence");
					return false;
				}
				unsigned int v = c - '0';
				for (int n = 1; n < 3 && i < len &&
				    data[i] >= '0' && data[i] <= '7'; n++)
					v = (v << 3) | (unsigned int)(data[i++] - '0');
				if (v == 0 || v > 255) {
					xfree(value);
					xpm_error(cause, "invalid octal escape");
					return false;
				}
				string_append(&value, &out, &capacity, (char)v);
				break;
			}
		}
		if (!closed) {
			xfree(value);
			xpm_error(cause, "unterminated string");
			return false;
		}
		string_append(&value, &out, &capacity, '\0');
		strings_add(strings, value);
	}
	if (strings->count == 0) {
		xpm_error(cause, "no XPM strings found");
		return false;
	}
	return true;
}

static bool
parse_uint(const char **cursor, unsigned int *value)
{
	while (isspace((unsigned char)**cursor))
		(*cursor)++;
	if (!isdigit((unsigned char)**cursor))
		return false;
	errno = 0;
	char *end;
	unsigned long result = strtoul(*cursor, &end, 10);
	if (errno != 0 || result > UINT32_MAX)
		return false;
	*cursor = end;
	*value = (unsigned int)result;
	return true;
}

static bool
field_key(const char *start, size_t len)
{
	return (len == 1 && strchr("smgc", start[0]) != NULL) ||
	    (len == 2 && start[0] == 'g' && start[1] == '4');
}

static char *
colour_field(const char *line, unsigned int cpp, const char *wanted)
{
	const char *p = line + cpp;
	while (*p != '\0') {
		while (isspace((unsigned char)*p))
			p++;
		const char *key = p;
		while (*p != '\0' && !isspace((unsigned char)*p))
			p++;
		size_t key_len = (size_t)(p - key);
		if (!field_key(key, key_len))
			return NULL;
		while (isspace((unsigned char)*p))
			p++;
		const char *value = p;
		const char *end = p + strlen(p);
		const char *scan = p;
		while (*scan != '\0') {
			while (*scan != '\0' && !isspace((unsigned char)*scan))
				scan++;
			const char *space = scan;
			while (isspace((unsigned char)*scan))
				scan++;
			const char *token = scan;
			while (*scan != '\0' && !isspace((unsigned char)*scan))
				scan++;
			if (field_key(token, (size_t)(scan - token))) {
				end = space;
				break;
			}
		}
		while (end > value && isspace((unsigned char)end[-1]))
			end--;
		if (strlen(wanted) == key_len &&
		    strncmp(key, wanted, key_len) == 0 && end > value) {
			size_t len = (size_t)(end - value);
			char *result = xmalloc(len + 1);
			memcpy(result, value, len);
			result[len] = '\0';
			return result;
		}
		p = end;
	}
	return NULL;
}

static bool
parse_hex_colour(const char *value, unsigned char *rgba)
{
	size_t len = strlen(value + 1);
	if (len == 0 || len % 3 != 0 || len / 3 > 4)
		return false;
	size_t digits = len / 3;
	unsigned int max = (1U << (digits * 4)) - 1;
	for (int channel = 0; channel < 3; channel++) {
		unsigned int v = 0;
		for (size_t n = 0; n < digits; n++) {
			int digit = hex_digit((unsigned char)value[1 +
			    (size_t)channel * digits + n]);
			if (digit < 0)
				return false;
			v = (v << 4) | (unsigned int)digit;
		}
		rgba[channel] = (unsigned char)((v * 255U + max / 2) / max);
	}
	rgba[3] = 255;
	return true;
}

static bool
parse_gray(const char *value, unsigned char *rgba)
{
	const char *number = NULL;
	if (strncasecmp(value, "gray", 4) == 0)
		number = value + 4;
	else if (strncasecmp(value, "grey", 4) == 0)
		number = value + 4;
	if (number == NULL || !isdigit((unsigned char)*number))
		return false;
	char *end;
	long percentage = strtol(number, &end, 10);
	if (*end != '\0' || percentage < 0 || percentage > 100)
		return false;
	unsigned char v = (unsigned char)((percentage * 255 + 50) / 100);
	rgba[0] = rgba[1] = rgba[2] = v;
	rgba[3] = 255;
	return true;
}

static bool
parse_colour(const char *value, unsigned char *rgba)
{
	if (strcasecmp(value, "none") == 0) {
		memset(rgba, 0, 4);
		return true;
	}
	if (value[0] == '#')
		return parse_hex_colour(value, rgba);
	if (parse_gray(value, rgba))
		return true;
	for (size_t i = 0; i < sizeof(named_colours) /
	    sizeof(named_colours[0]); i++) {
		if (strcasecmp(value, named_colours[i].name) != 0)
			continue;
		rgba[0] = named_colours[i].r;
		rgba[1] = named_colours[i].g;
		rgba[2] = named_colours[i].b;
		rgba[3] = 255;
		return true;
	}
	return false;
}

static uint32_t
key_hash(const char *key, unsigned int cpp)
{
	uint32_t hash = 2166136261U;
	for (unsigned int i = 0; i < cpp; i++) {
		hash ^= (unsigned char)key[i];
		hash *= 16777619U;
	}
	return hash;
}

static bool
decode_xpm(struct xpm_strings *strings, struct cow_xpm_image *image,
    char **cause)
{
	const char *header = strings->items[0];
	unsigned int width, height, ncolours, cpp;
	if (!parse_uint(&header, &width) || !parse_uint(&header, &height) ||
	    !parse_uint(&header, &ncolours) || !parse_uint(&header, &cpp) ||
	    width == 0 || height == 0 || ncolours == 0 || cpp == 0) {
		xpm_error(cause, "invalid XPM header");
		return false;
	}
	if (cpp > XPM_MAX_CPP || ncolours > XPM_MAX_COLOURS ||
	    (size_t)width > XPM_MAX_PIXELS / height) {
		xpm_error(cause, "XPM dimensions or palette are too large");
		return false;
	}
	if (strings->count < (size_t)1 + ncolours + height) {
		xpm_error(cause, "XPM data is incomplete");
		return false;
	}

	struct xpm_colour *palette = xcalloc(ncolours, sizeof(*palette));
	size_t table_size = 1;
	while (table_size < (size_t)ncolours * 2)
		table_size <<= 1;
	unsigned int *table = xcalloc(table_size, sizeof(*table));
	bool ok = false;

	for (unsigned int i = 0; i < ncolours; i++) {
		const char *line = strings->items[1 + i];
		if (strlen(line) < cpp) {
			xpm_error(cause, "XPM colour %u has a short key", i + 1);
			goto out;
		}
		memcpy(palette[i].key, line, cpp);
		char *value = colour_field(line, cpp, "c");
		if (value == NULL)
			value = colour_field(line, cpp, "g");
		if (value == NULL)
			value = colour_field(line, cpp, "g4");
		if (value == NULL)
			value = colour_field(line, cpp, "m");
		if (value == NULL || !parse_colour(value, palette[i].rgba)) {
			xpm_error(cause, "XPM colour %u is unsupported: %s", i + 1,
			    value != NULL ? value : "missing colour field");
			xfree(value);
			goto out;
		}
		xfree(value);

		size_t slot = key_hash(palette[i].key, cpp) & (table_size - 1);
		while (table[slot] != 0) {
			unsigned int previous = table[slot] - 1;
			if (memcmp(palette[previous].key, palette[i].key, cpp) == 0) {
				xpm_error(cause, "duplicate XPM colour key");
				goto out;
			}
			slot = (slot + 1) & (table_size - 1);
		}
		table[slot] = i + 1;
	}

	image->pixels = xcalloc((size_t)width * height, 4);
	image->width = (int)width;
	image->height = (int)height;
	for (unsigned int y = 0; y < height; y++) {
		const char *row = strings->items[1 + ncolours + y];
		if (strlen(row) != (size_t)width * cpp) {
			xpm_error(cause, "XPM row %u has the wrong width", y + 1);
			goto out;
		}
		for (unsigned int x = 0; x < width; x++) {
			const char *key = row + (size_t)x * cpp;
			size_t slot = key_hash(key, cpp) & (table_size - 1);
			while (table[slot] != 0) {
				unsigned int index = table[slot] - 1;
				if (memcmp(palette[index].key, key, cpp) == 0) {
					memcpy(image->pixels +
					    ((size_t)y * width + x) * 4,
					    palette[index].rgba, 4);
					break;
				}
				slot = (slot + 1) & (table_size - 1);
			}
			if (table[slot] == 0) {
				xpm_error(cause,
				    "XPM row %u uses an unknown colour key", y + 1);
				goto out;
			}
		}
	}
	ok = true;

out:
	if (!ok)
		cow_xpm_free(image);
	xfree(table);
	xfree(palette);
	return ok;
}

bool
cow_xpm_load(const char *path, struct cow_xpm_image *image, char **cause)
{
	memset(image, 0, sizeof(*image));
	if (cause != NULL)
		*cause = NULL;
	FILE *fp = fopen(path, "rb");
	if (fp == NULL) {
		xpm_error(cause, "%s: %s", path, strerror(errno));
		return false;
	}
	if (fseek(fp, 0, SEEK_END) != 0) {
		xpm_error(cause, "%s: cannot seek", path);
		fclose(fp);
		return false;
	}
	long length = ftell(fp);
	if (length < 0 || (unsigned long)length > XPM_MAX_FILE_SIZE ||
	    fseek(fp, 0, SEEK_SET) != 0) {
		xpm_error(cause, "%s: file is too large or cannot be read", path);
		fclose(fp);
		return false;
	}
	char *data = xmalloc((size_t)length + 1);
	if (fread(data, 1, (size_t)length, fp) != (size_t)length) {
		xpm_error(cause, "%s: cannot read file", path);
		xfree(data);
		fclose(fp);
		return false;
	}
	data[length] = '\0';
	fclose(fp);

	struct xpm_strings strings = { 0 };
	bool ok = extract_strings(data, (size_t)length, &strings, cause) &&
	    decode_xpm(&strings, image, cause);
	if (!ok && cause != NULL && *cause != NULL) {
		char *detail = *cause;
		xasprintf(cause, "%s: %s", path, detail);
		xfree(detail);
	}
	strings_free(&strings);
	xfree(data);
	return ok;
}

void
cow_xpm_free(struct cow_xpm_image *image)
{
	if (image == NULL)
		return;
	xfree(image->pixels);
	memset(image, 0, sizeof(*image));
}
