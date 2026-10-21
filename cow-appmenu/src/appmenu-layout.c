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
#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "cow/memory.h"
#include "appmenu-layout.h"

struct item {
	const cJSON *object;
	const char *label;
	xmlNode *placement;
	int submenu, used;
};

static int
is_node(const xmlNode *node, const char *name)
{
	return node && node->type == XML_ELEMENT_NODE &&
	    xmlStrEqual(node->name, BAD_CAST name);
}

static const char *
string(const cJSON *object, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);

	return cJSON_IsString(value) ? value->valuestring : "";
}

static int
limit_value(const char *value)
{
	char *end;
	unsigned long n;

	if (!*value || strspn(value, "0123456789") != strlen(value))
		return -1;
	errno = 0;
	n = strtoul(value, &end, 10);
	return errno || *end || n > INT_MAX ? -1 : (int)n;
}

static int
attributes_valid(xmlNode *node)
{
	for (xmlAttr *attr = node->properties; attr; attr = attr->next) {
		xmlChar *value = xmlNodeListGetString(node->doc, attr->children, 1);
		int valid = 0;

		if (!attr->ns && value) {
			if (is_node(node, "Merge") &&
			    xmlStrEqual(attr->name, BAD_CAST "type"))
				valid = !strcmp((char *)value, "menus") ||
				    !strcmp((char *)value, "files") ||
				    !strcmp((char *)value, "all");
			else if (is_node(node, "DefaultLayout") || is_node(node, "Menuname")) {
				if (xmlStrEqual(attr->name, BAD_CAST "show_empty") ||
				    xmlStrEqual(attr->name, BAD_CAST "inline") ||
				    xmlStrEqual(attr->name, BAD_CAST "inline_header") ||
				    xmlStrEqual(attr->name, BAD_CAST "inline_alias"))
					valid = !strcmp((char *)value, "true") ||
					    !strcmp((char *)value, "false");
				else if (xmlStrEqual(attr->name, BAD_CAST "inline_limit"))
					valid = limit_value((char *)value) >= 0;
			}
		}
		xmlFree(value);
		if (!valid)
			return 0;
	}
	return 1;
}

int
appmenu_layout_validate(xmlNode *node)
{
	int menus = 0, files = 0, count = 0;

	if (node->ns || node->nsDef || !attributes_valid(node))
		goto invalid;
	for (xmlNode *child = node->children; child; child = child->next) {
		int text;
		xmlChar *value;

		if (child->type == XML_COMMENT_NODE ||
		    (child->type == XML_TEXT_NODE && xmlIsBlankNode(child)))
			continue;
		if (child->type != XML_ELEMENT_NODE || child->ns || child->nsDef ||
		    !attributes_valid(child))
			goto invalid;
		text = is_node(child, "Filename") || is_node(child, "Menuname");
		if (!text && !is_node(child, "Separator") && !is_node(child, "Merge"))
			goto invalid;
		for (xmlNode *part = child->children; part; part = part->next) {
			if (part->type == XML_COMMENT_NODE)
				continue;
			if (part->type != XML_TEXT_NODE || (!text && !xmlIsBlankNode(part)))
				goto invalid;
		}
		value = xmlNodeGetContent(child);
		if (text && (!value || !*value ||
		    (is_node(child, "Menuname") && strchr((char *)value, '/')))) {
			xmlFree(value);
			goto invalid;
		}
		xmlFree(value);
		if (is_node(child, "Merge")) {
			value = xmlGetProp(child, BAD_CAST "type");
			if (!value)
				goto invalid;
			menus += !xmlStrEqual(value, BAD_CAST "files");
			files += !xmlStrEqual(value, BAD_CAST "menus");
			xmlFree(value);
		}
		count++;
	}
	if (!count || (menus == 1 && files == 1))
		return 1;
invalid:
	fprintf(stderr, "cow-appmenu: menu line %ld: invalid or unsupported layout\n",
	    xmlGetLineNo(node));
	return 0;
}

static xmlNode *
last_child(xmlNode *node, const char *name)
{
	xmlNode *found = NULL;

	for (xmlNode *child = node->children; child; child = child->next) {
		if (is_node(child, name))
			found = child;
	}
	return found;
}

static int
option(xmlNode *node, const char *name, int fallback)
{
	xmlChar *value = node ? xmlGetProp(node, BAD_CAST name) : NULL;
	int result = fallback;

	if (value)
		result = !strcmp(name, "inline_limit") ? limit_value((char *)value) :
		    xmlStrEqual(value, BAD_CAST "true");

	xmlFree(value);
	return result;
}

static const char *
item_name(const struct item *item)
{
	const char *id = string(item->object, "id");
	const char *slash = item->submenu ? strrchr(id, '/') : NULL;

	return slash ? slash + 1 : id;
}

static xmlNode *
mention(xmlNode *layout, const struct item *item)
{
	if (!layout)
		return NULL;
	for (xmlNode *child = layout->children; child; child = child->next) {
		xmlChar *name;
		int match;

		if (!is_node(child, item->submenu ? "Menuname" : "Filename"))
			continue;
		name = xmlNodeGetContent(child);
		match = name && !strcmp((char *)name, item_name(item));
		xmlFree(name);
		if (match)
			return child;
	}
	return NULL;
}

static int
compare(const void *a, const void *b)
{
	const struct item *left = a, *right = b;
	int result = strcoll(left->label, right->label);

	if (!result)
		result = strcmp(string(left->object, "id"), string(right->object, "id"));
	if (!result)
		result = left->submenu - right->submenu;
	return result;
}

static void
append(cJSON *entries, struct item *item, xmlNode *defaults)
{
	cJSON *entry;
	const cJSON *children = cJSON_GetObjectItemCaseSensitive(item->object, "entries");
	int count = cJSON_GetArraySize(children);
	int empty = option(item->placement, "show_empty", option(defaults, "show_empty", 0));
	int inline_menu = option(item->placement, "inline", option(defaults, "inline", 0));
	int limit = option(item->placement, "inline_limit", option(defaults, "inline_limit", 4));
	int alias = option(item->placement, "inline_alias", option(defaults, "inline_alias", 0));
	int header = option(item->placement, "inline_header", option(defaults, "inline_header", 1));

	if (item->used)
		return;
	item->used = 1;
	if (item->submenu && !empty && !count)
		return;
	if (item->submenu && count && inline_menu && (!limit || count <= limit)) {
		const cJSON *child;

		if (header && !(alias && count == 1)) {
			entry = cJSON_CreateObject();
			cJSON_AddStringToObject(entry, "type", "header");
			cJSON_AddStringToObject(entry, "name", item->label);
			cJSON_AddItemToArray(entries, entry);
		}
		cJSON_ArrayForEach(child, children) {
			entry = cJSON_Duplicate(child, 1);
			if (alias && count == 1) {
				cJSON_DeleteItemFromObjectCaseSensitive(entry, "name");
				cJSON_AddStringToObject(entry, "name", item->label);
			}
			cJSON_AddItemToArray(entries, entry);
		}
		return;
	}
	entry = cJSON_CreateObject();
	cJSON_AddStringToObject(entry, "type", item->submenu ? "menu" : "application");
	cJSON_AddStringToObject(entry, "id", string(item->object, "id"));
	if (!item->submenu)
		cJSON_AddStringToObject(entry, "file", string(item->object, "file"));
	cJSON_AddItemToArray(entries, entry);
}

void
appmenu_layout(xmlNode *node, cJSON *menu, const cJSON *catalogue, xmlNode *inherited)
{
	xmlNode *defaults = last_child(node, "DefaultLayout");
	xmlNode *layout = last_child(node, "Layout");
	cJSON *children = cJSON_GetObjectItemCaseSensitive(menu, "menus");
	cJSON *apps = cJSON_GetObjectItemCaseSensitive(menu, "applications");
	cJSON *entries, *object;
	size_t count = cJSON_GetArraySize(children) + cJSON_GetArraySize(apps), n = 0;
	struct item *items = xcalloc(count ? count : 1, sizeof(*items));

	if (!defaults)
		defaults = inherited;
	if (!layout || !xmlFirstElementChild(layout))
		layout = defaults;
	cJSON_ArrayForEach(object, children) {
		const char *id = string(object, "id");
		const char *name = strrchr(id, '/') + 1;

		for (xmlNode *child = node->children; child; child = child->next) {
			xmlNode *label;
			xmlChar *value;
			int match;

			if (!is_node(child, "Menu"))
				continue;
			label = last_child(child, "Name");
			value = xmlNodeGetContent(label);
			match = value && !strcmp((char *)value, name);
			xmlFree(value);
			if (match) {
				appmenu_layout(child, object, catalogue, defaults);
				break;
			}
		}
		items[n++] = (struct item){.object = object,
		    .label = string(object, "name"), .submenu = 1};
	}
	cJSON_ArrayForEach(object, apps) {
		const cJSON *app;
		const char *label = string(object, "id");

		cJSON_ArrayForEach(app, catalogue) {
			if (!strcmp(string(app, "id"), string(object, "id")) &&
			    !strcmp(string(app, "file"), string(object, "file"))) {
				label = string(app, "Name");
				break;
			}
		}
		items[n++] = (struct item){.object = object, .label = label};
	}
	qsort(items, count, sizeof(*items), compare);
	for (size_t i = 0; i < count; i++)
		items[i].placement = mention(layout, &items[i]);
	entries = cJSON_AddArrayToObject(menu, "entries");
	if (!layout || !xmlFirstElementChild(layout)) {
		for (int submenu = 1; submenu >= 0; submenu--) {
			for (size_t i = 0; i < count; i++) {
				if (items[i].submenu == submenu)
					append(entries, &items[i], defaults);
			}
		}
	}
	for (xmlNode *child = layout ? layout->children : NULL; child; child = child->next) {
		if (is_node(child, "Separator")) {
			if (entries->child &&
			    strcmp(string(entries->child->prev, "type"), "separator")) {
				cJSON *separator = cJSON_CreateObject();

				cJSON_AddStringToObject(separator, "type", "separator");
				cJSON_AddItemToArray(entries, separator);
			}
		} else if (is_node(child, "Merge")) {
			xmlChar *type = xmlGetProp(child, BAD_CAST "type");

			for (size_t i = 0; i < count; i++) {
				if (!items[i].placement &&
				    !xmlStrEqual(type, BAD_CAST (items[i].submenu ? "files" : "menus")))
					append(entries, &items[i], defaults);
			}
			xmlFree(type);
		} else {
			for (size_t i = 0; i < count; i++) {
				if (items[i].placement == child)
					append(entries, &items[i], defaults);
			}
		}
	}
	if (entries->child && !strcmp(string(entries->child->prev, "type"), "separator"))
		cJSON_Delete(cJSON_DetachItemViaPointer(entries, entries->child->prev));
	free(items);
}
