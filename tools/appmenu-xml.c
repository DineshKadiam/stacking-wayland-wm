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

#define _XOPEN_SOURCE 700
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libxml/parser.h>

#include "cow/memory.h"
#include "appmenu-entry.h"
#include "appmenu-xml.h"

/* Bound recursive includes and the aggregate XML input before expanding it. */
#define MENU_MAX_DEPTH 64
#define MENU_MAX_FILES 256
#define MENU_MAX_BYTES (16 * 1024 * 1024)

struct loader {
	int (*validate)(xmlNode *, unsigned);
	const char *stack[MENU_MAX_DEPTH];
	unsigned stack_size;
	unsigned files;
	size_t bytes;
	int failed;
	cJSON *config_dirs;
};

static xmlDoc *load(struct loader *, const char *, unsigned, int);

static int
is_node(const xmlNode *node, const char *name)
{
	return node->type == XML_ELEMENT_NODE &&
	    xmlStrEqual(node->name, BAD_CAST name);
}

int
appmenu_parent_merge(const xmlNode *node)
{
	xmlChar *type;
	int parent;

	if (!is_node(node, "MergeFile"))
		return 0;
	type = xmlGetProp(node, BAD_CAST "type");
	parent = type && xmlStrEqual(type, BAD_CAST "parent");
	xmlFree(type);
	return parent;
}

static void
failure(struct loader *loader, const char *path, const char *reason)
{
	fprintf(stderr, "cow-appmenu: %s: %s\n", path, reason);
	loader->failed = 1;
}

static xmlDoc *
read_document(struct loader *loader, const char *path)
{
	struct stat st;
	int fd = open(path, O_RDONLY | O_NONBLOCK);
	FILE *fp;
	char *data;
	size_t size;
	xmlDoc *doc = NULL;

	if (fd < 0) {
		failure(loader, path, strerror(errno));
		return NULL;
	}
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
	    st.st_size < 0 || (uintmax_t)st.st_size > MENU_MAX_BYTES - loader->bytes) {
		failure(loader, path, "not a regular file or XML input limit exceeded");
		close(fd);
		return NULL;
	}
	fp = fdopen(fd, "rb");
	if (!fp) {
		failure(loader, path, strerror(errno));
		close(fd);
		return NULL;
	}
	size = (size_t)st.st_size;
	data = xmalloc(size + 1);
	data[size] = '\0';
	if (fread(data, 1, size, fp) != size || fgetc(fp) != EOF || ferror(fp)) {
		failure(loader, path, "cannot read a stable menu file");
	} else {
		loader->bytes += size;
		/* Do not load DTDs, substitute entities or resolve external URLs. */
		doc = xmlReadMemory(data, (int)size, path, NULL, XML_PARSE_NONET);
		if (!doc)
			failure(loader, path, "invalid XML");
	}
	free(data);
	fclose(fp);
	return doc;
}

static const char *
relative_to(const char *file, const char *dir)
{
	size_t len = strlen(dir);

	while (len && dir[len - 1] == '/')
		len--;
	if (strncmp(file, dir, len) == 0 && file[len] == '/')
		return file + len + 1;
	return NULL;
}

static char *
parent_path(struct loader *loader, const char *source)
{
	char *path = NULL, *canonical = realpath(source, NULL);

	/*
	 * Prefer the location requested by the caller, preserving symlinked user
	 * menus. Canonical paths are a fallback for spelling differences.
	 */
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < cJSON_GetArraySize(loader->config_dirs); i++) {
			const char *dir = cJSON_GetArrayItem(loader->config_dirs, i)->valuestring;
			char *resolved = pass ? realpath(dir, NULL) : NULL;
			const char *relative = NULL;

			if (!pass)
				relative = relative_to(source, dir);
			else if (resolved && canonical)
				relative = relative_to(canonical, resolved);
			if (relative) {
				int found = appmenu_find_file(loader->config_dirs,
				    relative, i + 1, &path);

				if (found < 0)
					loader->failed = 1;
				free(resolved);
				free(canonical);
				return path;
			}
			free(resolved);
		}
	}
	free(canonical);
	return NULL;
}

static void
expand_defaults(struct loader *loader, xmlNode *menu, const char *source)
{
	xmlNode *node, *next;

	for (node = menu->children; node; node = next) {
		next = node->next;
		if (appmenu_parent_merge(node)) {
			char *path = parent_path(loader, source);

			if (path) {
				xmlNodeSetContent(node, NULL);
				xmlAddChild(node, xmlNewText(BAD_CAST path));
				xmlUnsetProp(node, BAD_CAST "type");
				free(path);
			} else {
				xmlUnlinkNode(node);
				xmlFreeNode(node);
			}
		} else if (is_node(node, "DefaultMergeDirs")) {
			const char *name = strrchr(source, '/');
			const char *prefix = getenv("XDG_MENU_PREFIX");
			char *stem;
			size_t len;

			name = name ? name + 1 : source;
			if (prefix && *prefix && strncmp(name, prefix, strlen(prefix)) == 0)
				name += strlen(prefix);
			stem = xstrdup(name);
			len = strlen(stem);
			if (len > 5 && strcmp(stem + len - 5, ".menu") == 0)
				stem[len - 5] = '\0';
			for (int i = cJSON_GetArraySize(loader->config_dirs) - 1; i >= 0; i--) {
				const char *dir = cJSON_GetArrayItem(loader->config_dirs, i)->valuestring;
				char *path;
				xmlNode *merge = xmlNewNode(NULL, BAD_CAST "MergeDir");

				xasprintf(&path, "%s/%s-merged", dir, stem);
				xmlAddChild(merge, xmlNewText(BAD_CAST path));
				xmlAddPrevSibling(node, merge);
				free(path);
			}
			free(stem);
			xmlUnlinkNode(node);
			xmlFreeNode(node);
		}
	}
}

static void
absolute_paths(xmlNode *menu, const char *base)
{
	for (xmlNode *node = menu->children; node; node = node->next) {
		xmlChar *text;
		char *path, *canonical;

		if (!is_node(node, "AppDir") && !is_node(node, "DirectoryDir") &&
		    !is_node(node, "MergeFile") && !is_node(node, "MergeDir"))
			continue;
		text = xmlNodeGetContent(node);
		if (*text == '/')
			path = xstrdup((char *)text);
		else
			xasprintf(&path, "%s%s", base, text);
		canonical = realpath(path, NULL);
		/* xmlNewText keeps literal '&' in paths, rather than parsing entities. */
		xmlNodeSetContent(node, NULL);
		xmlAddChild(node, xmlNewText(BAD_CAST (canonical ? canonical : path)));
		free(canonical);
		free(path);
		xmlFree(text);
	}
}

static int
repeated_later(xmlNode *node)
{
	xmlChar *path = xmlNodeGetContent(node);
	int found = 0;

	for (xmlNode *other = node->next; other; other = other->next) {
		xmlChar *next;

		if (!is_node(other, (char *)node->name))
			continue;
		next = xmlNodeGetContent(other);
		found = xmlStrEqual(path, next);
		xmlFree(next);
		if (found)
			break;
	}
	xmlFree(path);
	return found;
}

static void
merge_file(struct loader *loader, xmlNode *at, const char *path, unsigned depth)
{
	xmlDoc *doc = load(loader, path, depth + 1, 1);

	if (!doc)
		return;
	for (xmlNode *node = xmlDocGetRootElement(doc)->children; node; node = node->next) {
		if (!is_node(node, "Name"))
			xmlAddPrevSibling(at, xmlDocCopyNode(node, at->doc, 1));
	}
	xmlFreeDoc(doc);
}

static void
merge_directory(struct loader *loader, xmlNode *at, const char *path,
    unsigned depth)
{
	struct dirent **entries;
	int count = scandir(path, &entries, NULL, alphasort);

	if (count < 0) {
		if (errno != ENOENT && errno != ENOTDIR)
			failure(loader, path, strerror(errno));
		return;
	}
	for (int i = 0; i < count; i++) {
		size_t len = strlen(entries[i]->d_name);

		if (!loader->failed && len > 5 &&
		    strcmp(entries[i]->d_name + len - 5, ".menu") == 0) {
			char *file;

			xasprintf(&file, "%s/%s", path, entries[i]->d_name);
			merge_file(loader, at, file, depth);
			free(file);
		}
		free(entries[i]);
	}
	free(entries);
}

static void
expand(struct loader *loader, xmlNode *menu, const char *base,
    const char *source, unsigned depth)
{
	xmlNode *node, *next;

	if (depth >= MENU_MAX_DEPTH) {
		failure(loader, base, "menu nesting limit exceeded");
		return;
	}
	expand_defaults(loader, menu, source);
	absolute_paths(menu, base);
	for (node = menu->children; node && !loader->failed; node = next) {
		next = node->next;
		if (is_node(node, "Menu")) {
			expand(loader, node, base, source, depth + 1);
		} else if (is_node(node, "MergeFile") || is_node(node, "MergeDir")) {
			if (!repeated_later(node)) {
				xmlChar *path = xmlNodeGetContent(node);

				if (is_node(node, "MergeFile"))
					merge_file(loader, node, (char *)path, depth);
				else
					merge_directory(loader, node, (char *)path, depth);
				xmlFree(path);
			}
			xmlUnlinkNode(node);
			xmlFreeNode(node);
		}
	}
}

static xmlDoc *
load(struct loader *loader, const char *path, unsigned depth, int optional)
{
	char *canonical = realpath(path, NULL), *base, *source = NULL;
	xmlDoc *doc = NULL;
	xmlNode *root;

	if (!canonical) {
		if (!optional || (errno != ENOENT && errno != ENOTDIR))
			failure(loader, path, strerror(errno));
		return NULL;
	}
	if (depth >= MENU_MAX_DEPTH || loader->files++ >= MENU_MAX_FILES) {
		failure(loader, path, "menu include limit exceeded");
		goto out;
	}
	for (unsigned i = 0; i < loader->stack_size; i++) {
		if (strcmp(loader->stack[i], canonical) == 0) {
			failure(loader, path, "recursive menu include");
			goto out;
		}
	}
	doc = read_document(loader, canonical);
	if (!doc)
		goto out;
	root = xmlDocGetRootElement(doc);
	if (doc->intSubset && doc->intSubset->children)
		failure(loader, path, "internal DTD declarations are not supported");
	else if (!root || !is_node(root, "Menu"))
		failure(loader, path, "expected a Menu root");
	else if (!loader->validate(root, 0))
		loader->failed = 1;
	if (!loader->failed) {
		if (path[0] == '/') {
			source = xstrdup(path);
		} else {
			char *cwd = realpath(".", NULL);

			if (!cwd) {
				failure(loader, path, strerror(errno));
				goto out;
			}
			xasprintf(&source, "%s/%s", cwd, path);
			free(cwd);
		}
		base = xstrdup(source);
		strrchr(base, '/')[1] = '\0';
		loader->stack[loader->stack_size++] = canonical;
		expand(loader, root, base, source, depth);
		loader->stack_size--;
		free(base);
	}
out:
	if (loader->failed && doc) {
		xmlFreeDoc(doc);
		doc = NULL;
	}
	free(canonical);
	free(source);
	return doc;
}

static xmlChar *
menu_name(xmlNode *menu)
{
	for (xmlNode *node = menu->children; node; node = node->next) {
		if (is_node(node, "Name"))
			return xmlNodeGetContent(node);
	}
	return NULL;
}

static void
merge_menu(xmlNode *source, xmlNode *destination)
{
	xmlNode *child, *next, *first = xmlFirstElementChild(destination);
	xmlUnlinkNode(source);
	for (child = source->children; child; child = next) {
		next = child->next;
		xmlUnlinkNode(child);
		if (is_node(child, "Name"))
			xmlFreeNode(child);
		else
			xmlAddPrevSibling(first, child);
	}
	xmlFreeNode(source);
}

static void
consolidate(xmlNode *menu)
{
	xmlNode *node, *next;

	/* Merge into the next namesake; repeated passes retain the final position. */
	for (node = menu->children; node; node = next) {
		xmlChar *name;

		next = node->next;
		if (!is_node(node, "Menu"))
			continue;
		name = menu_name(node);
		for (xmlNode *other = next; other; other = other->next) {
			xmlChar *other_name;
			int same;

			if (!is_node(other, "Menu"))
				continue;
			other_name = menu_name(other);
			same = xmlStrEqual(name, other_name);
			xmlFree(other_name);
			if (same) {
				merge_menu(node, other);
				break;
			}
		}
		xmlFree(name);
	}
	for (node = menu->children; node; node = node->next) {
		if (is_node(node, "Menu"))
			consolidate(node);
	}
}

static xmlNode *
menu_path(xmlNode *menu, const char *path, int create)
{
	char *copy = xstrdup(path), *part, *save = NULL;
	for (part = strtok_r(copy, "/", &save); part && menu;
	    part = strtok_r(NULL, "/", &save)) {
		xmlNode *child;
		for (child = menu->children; child; child = child->next) {
			if (!is_node(child, "Menu"))
				continue;
			xmlChar *name = menu_name(child);
			int same = xmlStrEqual(name, BAD_CAST part);
			xmlFree(name);
			if (same)
				break;
		}
		if (!child && create) {
			child = xmlNewChild(menu, NULL, BAD_CAST "Menu", NULL);
			xmlNewTextChild(child, NULL, BAD_CAST "Name", BAD_CAST part);
		}
		menu = child;
	}
	free(copy);
	return menu;
}

static int
apply_moves(xmlNode *menu, int (*validate)(xmlNode *, unsigned))
{
	xmlNode *node;
	/* Child moves precede parent moves, regardless of XML position. */
	for (node = menu->children; node; node = node->next)
		if (is_node(node, "Menu") && !apply_moves(node, validate))
			return 0;
	for (;;) {
		/* Moves may remove or merge the following sibling. */
		for (node = menu->children; node; node = node->next)
			if (is_node(node, "Move"))
				break;
		if (!node)
			break;
		for (xmlNode *old = xmlFirstElementChild(node); old;
		    old = xmlNextElementSibling(xmlNextElementSibling(old))) {
			xmlChar *from = xmlNodeGetContent(old);
			xmlChar *to = xmlNodeGetContent(xmlNextElementSibling(old));
			xmlNode *source = menu_path(menu, (char *)from, 0);
			int cyclic = strncmp((char *)from, (char *)to,
			    strlen((char *)from)) == 0 &&
			    to[strlen((char *)from)] == '/';
			if (source && cyclic) {
				fprintf(stderr, "cow-appmenu: cannot move a menu into itself\n");
				xmlFree(from);
				xmlFree(to);
				return 0;
			}
			if (source && !xmlStrEqual(from, to)) {
				xmlNode *destination = menu_path(menu, (char *)to, 1);
				merge_menu(source, destination);
				/* Bound the changed tree before recursive consolidation. */
				if (!validate(xmlDocGetRootElement(menu->doc), 0)) {
					xmlFree(from);
					xmlFree(to);
					return 0;
				}
				consolidate(menu);
			}
			xmlFree(from);
			xmlFree(to);
		}
		xmlUnlinkNode(node);
		xmlFreeNode(node);
	}
	return 1;
}

xmlDoc *
appmenu_load(const char *path, int (*validate)(xmlNode *, unsigned))
{
	struct loader loader = {
		.validate = validate,
		.config_dirs = appmenu_config_dirs("menus"),
	};
	xmlDoc *doc = load(&loader, path, 0, 0);

	cJSON_Delete(loader.config_dirs);
	if (!doc)
		return NULL;
	consolidate(xmlDocGetRootElement(doc));
	if (!apply_moves(xmlDocGetRootElement(doc), validate) ||
	    !validate(xmlDocGetRootElement(doc), 0)) {
		xmlFreeDoc(doc);
		return NULL;
	}
	return doc;
}
