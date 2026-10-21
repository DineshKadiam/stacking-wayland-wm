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
#include <libxml/tree.h>

#include "cow/memory.h"
#include "appmenu-entry.h"
#include "appmenu-layout.h"
#include "appmenu-menu.h"
#include "appmenu-xml.h"

struct resolver {
	cJSON *catalogue;
	cJSON *pools;
	cJSON *allocated;
};

static int
is_node(const xmlNode *node, const char *name)
{
	return xmlStrEqual(node->name, BAD_CAST name);
}

static int
error_at(const xmlNode *node, const char *message)
{
	fprintf(stderr, "cow-appmenu: menu line %ld: %s (%s)\n",
	    xmlGetLineNo(node), message, node->name);
	return 0;
}

static int
is_rule(const xmlNode *node)
{
	return is_node(node, "All") || is_node(node, "Filename") ||
	    is_node(node, "Category") || is_node(node, "And") ||
	    is_node(node, "Or") || is_node(node, "Not");
}

static int
has_text(const xmlNode *node)
{
	return is_node(node, "Name") || is_node(node, "Filename") ||
	    is_node(node, "Category") || is_node(node, "Directory") ||
	    is_node(node, "DirectoryDir") || is_node(node, "AppDir") ||
	    is_node(node, "MergeFile") || is_node(node, "MergeDir") ||
	    is_node(node, "Old") || is_node(node, "New");
}

static int
directory_name_valid(const char *name)
{
	size_t len = strlen(name);
	const char *part = name;

	if (*name == '/' || len < 11 || strcmp(name + len - 10, ".directory"))
		return 0;
	while (*part) {
		size_t n = strcspn(part, "/");

		if (!n || (n == 1 && part[0] == '.') ||
		    (n == 2 && strncmp(part, "..", n) == 0))
			return 0;
		part += n;
		if (*part)
			part++;
	}
	return 1;
}

/*
 * Validate before evaluating, including branches which match no applications.
 * Unsupported menu features must not silently produce an incomplete launcher.
 */
static int
validate(xmlNode *node, unsigned depth)
{
	int names = 0;
	int moves = 0;
	int valid = 0;

	if (depth > 64) {
		error_at(node, "menu nesting is too deep");
		goto out;
	}
	if (node->ns || node->nsDef) {
		error_at(node, "namespaces are not supported");
		goto out;
	}
	if (is_node(node, "Layout") || is_node(node, "DefaultLayout"))
		return appmenu_layout_validate(node);
	for (xmlAttr *attr = node->properties; attr; attr = attr->next) {
		xmlChar *type;
		int supported;

		if (!is_node(node, "MergeFile") || attr->ns ||
		    !xmlStrEqual(attr->name, BAD_CAST "type")) {
			error_at(node, "unsupported attribute");
			goto out;
		}
		type = xmlNodeListGetString(node->doc, attr->children, 1);
		supported = type && (xmlStrEqual(type, BAD_CAST "path") ||
		    xmlStrEqual(type, BAD_CAST "parent"));
		xmlFree(type);
		if (!supported) {
			error_at(node, "unsupported MergeFile type");
			goto out;
		}
	}
	for (xmlNode *child = node->children; child; child = child->next) {
		if (child->type == XML_COMMENT_NODE)
			continue;
		if (child->type == XML_TEXT_NODE) {
			if (has_text(node) || xmlIsBlankNode(child))
				continue;
			error_at(node, "unexpected text");
			goto out;
		}
		if (child->type != XML_ELEMENT_NODE) {
			error_at(node, "entities and processing instructions are not supported");
			goto out;
		}
		if (is_node(node, "Menu")) {
			if (!is_node(child, "Menu") && !is_node(child, "Name") &&
			    !is_node(child, "Layout") && !is_node(child, "DefaultLayout") &&
			    !is_node(child, "DefaultAppDirs") &&
			    !is_node(child, "AppDir") &&
			    !is_node(child, "Directory") &&
			    !is_node(child, "DirectoryDir") &&
			    !is_node(child, "DefaultDirectoryDirs") &&
			    !is_node(child, "MergeFile") && !is_node(child, "MergeDir") &&
			    !is_node(child, "DefaultMergeDirs") &&
			    !is_node(child, "Move") &&
			    !is_node(child, "OnlyUnallocated") &&
			    !is_node(child, "NotOnlyUnallocated") &&
			    !is_node(child, "Include") && !is_node(child, "Exclude") &&
			    !is_node(child, "Deleted") && !is_node(child, "NotDeleted")) {
				error_at(child, "unsupported menu element");
				goto out;
			}
			if (is_node(child, "Name"))
				names++;
		} else if (is_node(node, "Move")) {
			if (!is_node(child, moves % 2 ? "New" : "Old")) {
				error_at(child, "Move requires Old/New pairs");
				goto out;
			}
			moves++;
		} else if (is_node(node, "Include") || is_node(node, "Exclude") ||
		    is_node(node, "And") || is_node(node, "Or") || is_node(node, "Not")) {
			if (!is_rule(child)) {
				error_at(child, "unsupported matching rule");
				goto out;
			}
		} else {
			error_at(node, "unexpected child element");
			goto out;
		}
		if (!validate(child, depth + 1))
			goto out;
	}
	if (is_node(node, "Menu") && names != 1) {
		error_at(node, "each menu needs exactly one Name");
		goto out;
	}
	if (is_node(node, "Move") && (!moves || moves % 2)) {
		error_at(node, "Move requires Old/New pairs");
		goto out;
	}
	if (has_text(node)) {
		xmlChar *text = xmlNodeGetContent(node);
		int bad = ((!text || !*text) && !appmenu_parent_merge(node)) ||
		    (is_node(node, "Name") && strchr((char *)text, '/')) ||
		    (is_node(node, "Directory") &&
		    !directory_name_valid((char *)text));
		if (!bad && (is_node(node, "Old") || is_node(node, "New"))) {
			const char *path = (char *)text;
			unsigned parts = 1;
			for (const char *p = path; *p; p++)
				if (*p == '/')
					parts++;
			bad = *path == '/' || path[strlen(path) - 1] == '/' ||
			    strstr(path, "//") != NULL || parts > 64;
		}

		xmlFree(text);
		if (bad) {
			error_at(node, "empty value or invalid name");
			goto out;
		}
	}
	valid = 1;
out:
	return valid;
}

static const char *
string(const cJSON *object, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);

	return cJSON_IsString(value) ? value->valuestring : "";
}

static int
has_category(const char *list, const char *category)
{
	char *word = xmalloc(strlen(list) + 1), *out = word;
	int match = 0;

	/* Semicolon lists have their own escaping, unlike display labels. */
	for (;;) {
		if (*list == '\\' && list[1]) {
			*out++ = *++list;
			list++;
		} else if (*list == ';' || !*list) {
			*out = '\0';
			if (strcmp(word, category) == 0)
				match = 1;
			out = word;
			if (!*list)
				break;
			list++;
		} else {
			*out++ = *list++;
		}
	}
	free(word);
	return match;
}

static int
matches(xmlNode *node, const cJSON *app)
{
	int all = 1, any = 0;

	if (is_node(node, "All"))
		return 1;
	if (is_node(node, "Filename") || is_node(node, "Category")) {
		xmlChar *text = xmlNodeGetContent(node);
		int result = is_node(node, "Filename") ?
		    strcmp(string(app, "id"), (char *)text) == 0 :
		    has_category(string(app, "Categories"), (char *)text);

		xmlFree(text);
		return result;
	}
	for (xmlNode *child = node->children; child; child = child->next) {
		int match;

		if (child->type != XML_ELEMENT_NODE)
			continue;
		match = matches(child, app);
		all = all && match;
		any = any || match;
	}
	if (is_node(node, "And"))
		return all;
	if (is_node(node, "Not"))
		return !any;
	return any;
}

static cJSON *
scope_dirs(xmlNode *node, const cJSON *inherited,
    const char *tag, const char *default_tag, const char *suffix)
{
	cJSON *dirs = cJSON_Duplicate(inherited, 1);

	for (xmlNode *child = node->children; child; child = child->next) {
		if (child->type != XML_ELEMENT_NODE)
			continue;
		if (is_node(child, tag)) {
			xmlChar *text = xmlNodeGetContent(child);
			cJSON_InsertItemInArray(dirs, 0, cJSON_CreateString((char *)text));
			xmlFree(text);
		} else if (is_node(child, default_tag)) {
			cJSON *defaults = appmenu_data_dirs(suffix);

			for (int i = cJSON_GetArraySize(defaults) - 1; i >= 0; i--)
				cJSON_InsertItemInArray(dirs, 0,
				    cJSON_Duplicate(cJSON_GetArrayItem(defaults, i), 1));
			cJSON_Delete(defaults);
		}
	}
	return dirs;
}

static cJSON *
directory_metadata(xmlNode *node, const cJSON *dirs)
{
	for (xmlNode *child = node->last; child; child = child->prev) {
		const cJSON *dir;
		xmlChar *name;

		if (child->type != XML_ELEMENT_NODE || !is_node(child, "Directory"))
			continue;
		name = xmlNodeGetContent(child);
		cJSON_ArrayForEach(dir, dirs) {
			char *path;
			cJSON *metadata;

			xasprintf(&path, "%s/%s", dir->valuestring, name);
			metadata = appmenu_directory(path);
			free(path);
			if (metadata) {
				xmlFree(name);
				return metadata;
			}
		}
		xmlFree(name);
	}
	return NULL;
}

static cJSON *
application_pool(struct resolver *resolver, const cJSON *dirs)
{
	cJSON *pool, *apps;

	cJSON_ArrayForEach(pool, resolver->pools) {
		if (cJSON_Compare(cJSON_GetObjectItemCaseSensitive(pool, "dirs"),
		    dirs, 1))
			return cJSON_GetObjectItemCaseSensitive(pool, "applications");
	}
	pool = cJSON_CreateObject();
	apps = appmenu_catalogue(dirs);
	cJSON_AddItemToObject(pool, "dirs", cJSON_Duplicate(dirs, 1));
	cJSON_AddItemToObject(pool, "applications", apps);
	cJSON_AddItemToArray(resolver->pools, pool);
	return apps;
}

/*
 * A desktop ID is unique within a search scope, not across all scopes.
 * Retain the source file in references so sibling overrides stay distinct.
 */
static cJSON *
reference(const cJSON *app)
{
	cJSON *ref = cJSON_CreateObject();

	cJSON_AddStringToObject(ref, "id", string(app, "id"));
	cJSON_AddStringToObject(ref, "file", string(app, "file"));
	return ref;
}

static int
contains_application(const cJSON *array, const cJSON *app)
{
	const cJSON *existing;

	cJSON_ArrayForEach(existing, array) {
		if (strcmp(string(existing, "id"), string(app, "id")) == 0 &&
		    strcmp(string(existing, "file"), string(app, "file")) == 0)
			return 1;
	}
	return 0;
}

static cJSON *
application_reference(struct resolver *resolver, const cJSON *app)
{
	if (!contains_application(resolver->catalogue, app))
		cJSON_AddItemToArray(resolver->catalogue, cJSON_Duplicate(app, 1));
	return reference(app);
}

static void
filter_unallocated(cJSON *menu, const cJSON *allocated)
{
	cJSON *items = cJSON_GetObjectItemCaseSensitive(menu, "applications");
	cJSON *children = cJSON_GetObjectItemCaseSensitive(menu, "menus");
	cJSON *item, *next;

	if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(menu, "_unallocated"))) {
		for (item = items->child; item; item = next) {
			next = item->next;
			if (contains_application(allocated, item))
				cJSON_Delete(cJSON_DetachItemViaPointer(items, item));
		}
	}
	cJSON_DeleteItemFromObjectCaseSensitive(menu, "_unallocated");
	cJSON_ArrayForEach(item, children)
		filter_unallocated(item, allocated);
}

static cJSON *
resolve(xmlNode *node, struct resolver *resolver, const char *parent,
    const cJSON *inherited_dirs, const cJSON *inherited_appdirs)
{
	xmlChar *name = NULL;
	int deleted = 0;
	int only_unallocated = 0;
	cJSON *menu, *children, *items;
	cJSON *dirs, *metadata, *appdirs, *apps;
	const cJSON *app;
	char *id;

	for (xmlNode *child = node->children; child; child = child->next) {
		if (child->type != XML_ELEMENT_NODE)
			continue;
		if (is_node(child, "Name"))
			name = xmlNodeGetContent(child);
		else if (is_node(child, "Deleted"))
			deleted = 1;
		else if (is_node(child, "NotDeleted"))
			deleted = 0;
		else if (is_node(child, "OnlyUnallocated"))
			only_unallocated = 1;
		else if (is_node(child, "NotOnlyUnallocated"))
			only_unallocated = 0;
	}
	dirs = scope_dirs(node, inherited_dirs,
	    "DirectoryDir", "DefaultDirectoryDirs", "desktop-directories");
	metadata = directory_metadata(node, dirs);
	if (deleted || (metadata &&
	    !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(metadata, "visible")))) {
		xmlFree(name);
		cJSON_Delete(dirs);
		cJSON_Delete(metadata);
		return NULL;
	}
	appdirs = scope_dirs(node, inherited_appdirs,
	    "AppDir", "DefaultAppDirs", "applications");
	apps = application_pool(resolver, appdirs);
	xasprintf(&id, "%s%s%s", parent, *parent ? "/" : "", name);
	menu = cJSON_CreateObject();
	cJSON_AddStringToObject(menu, "id", id);
	cJSON_AddBoolToObject(menu, "_unallocated", only_unallocated);
	cJSON_AddStringToObject(menu, "name",
	    metadata && *string(metadata, "name") ?
	    string(metadata, "name") : (char *)name);
	if (metadata) {
		cJSON_AddStringToObject(menu, "icon", string(metadata, "icon"));
		cJSON_AddStringToObject(menu, "comment", string(metadata, "comment"));
	}
	items = cJSON_AddArrayToObject(menu, "applications");
	children = cJSON_AddArrayToObject(menu, "menus");
	cJSON_ArrayForEach(app, apps) {
		int included = 0;

		for (xmlNode *child = node->children; child; child = child->next) {
			if (child->type != XML_ELEMENT_NODE)
				continue;
			if ((is_node(child, "Include") || is_node(child, "Exclude")) &&
			    matches(child, app)) {
				included = is_node(child, "Include");
				/* Later exclusion does not undo allocation. */
				if (included && !only_unallocated &&
				    !contains_application(resolver->allocated, app))
					cJSON_AddItemToArray(resolver->allocated, reference(app));
			}
		}
		if (included)
			cJSON_AddItemToArray(items, application_reference(resolver, app));
	}
	for (xmlNode *child = node->children; child; child = child->next) {
		cJSON *submenu;

		if (child->type != XML_ELEMENT_NODE || !is_node(child, "Menu"))
			continue;
		submenu = resolve(child, resolver, id, dirs, appdirs);
		if (submenu)
			cJSON_AddItemToArray(children, submenu);
	}
	free(id);
	xmlFree(name);
	cJSON_Delete(dirs);
	cJSON_Delete(metadata);
	cJSON_Delete(appdirs);
	return menu;
}

cJSON *
appmenu_resolve(const char *path, cJSON *applications)
{
	xmlDoc *doc = appmenu_load(path, validate);
	cJSON *result, *dirs, *pool;
	struct resolver resolver = {.catalogue = applications};

	if (!doc)
		return NULL;
	dirs = cJSON_CreateArray();
	pool = cJSON_CreateObject();
	resolver.pools = cJSON_CreateArray();
	resolver.allocated = cJSON_CreateArray();
	cJSON_AddItemToObject(pool, "dirs", appmenu_data_dirs("applications"));
	cJSON_AddItemToObject(pool, "applications", cJSON_Duplicate(applications, 1));
	cJSON_AddItemToArray(resolver.pools, pool);
	result = resolve(xmlDocGetRootElement(doc), &resolver, "", dirs, dirs);
	if (result) {
		filter_unallocated(result, resolver.allocated);
		appmenu_layout(xmlDocGetRootElement(doc), result, applications, NULL);
	}
	cJSON_Delete(resolver.allocated);
	cJSON_Delete(resolver.pools);
	cJSON_Delete(dirs);
	if (!result)
		result = cJSON_CreateNull();
	xmlFreeDoc(doc);
	return result;
}
