#include "themedoc.h"

#include "fs.h"
#include "imageio.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PREVIEW_HOME "preview_page.png"
#define PREVIEW_LOCK "preview_lockscreen.png"
#define PREVIEW_THUMB "preview_thumbnail.png"
#define GENERATED_ORIGIN (-1)

static const char *const part_names[PART_COUNT] = {"Wallpapers", "Music", "Icons", "Lock screen", "Status bar"};

// System app icon slots (some generators write m_maps instead of m_map).
static const char *const icon_slots[] = {
	"m_browser", "m_video", "m_music", "m_ps3Link", "m_party", "m_trophy", "m_map",
	"m_maps", "m_near", "m_hostCollabo", "m_welcomePark", "m_ps4Link", "m_friend", "m_email",
	"m_calendar", "m_store", "m_message", "m_parental", "m_camera", "m_settings", "m_power",
};

const char *themedoc_part_name(ThemePart part) {
	return part < PART_COUNT ? part_names[part] : "";
}

static int is_icon_slot(const char *name) {
	for (size_t i = 0; i < sizeof(icon_slots) / sizeof(icon_slots[0]); i++)
		if (strcmp(name, icon_slots[i]) == 0)
			return 1;
	return 0;
}

static XmlNode *copy_from(ThemeDoc *doc, const ThemeDoc *from, const XmlNode *src);
static void prepend(XmlNode *parent, XmlNode *child);
static XmlNode *ensure(XmlNode *parent, const char *name);

static int has_text(const XmlNode *n) {
	return n && n->text && n->text[0];
}

static void set_origin(XmlNode *n, int origin) {
	n->origin = origin;
	for (XmlNode *c = n->child; c; c = c->next)
		set_origin(c, origin);
}

static XmlNode *home(const ThemeDoc *doc) {
	return xml_find(doc->root, "HomeProperty");
}

int themedoc_load(ThemeDoc *doc, const char *dir) {
	memset(doc, 0, sizeof(*doc));
	char path[512], *text;
	snprintf(path, sizeof(path), "%s/theme.xml", dir);
	if (fs_read_file(path, &text, NULL) != 0)
		return -1;
	doc->root = xml_parse(text);
	free(text);
	if (!doc->root || strcmp(doc->root->name, "theme") != 0) {
		xml_free(doc->root);
		doc->root = NULL;
		return -1;
	}
	set_origin(doc->root, 0);
	snprintf(doc->origins[0], sizeof(doc->origins[0]), "%s", dir);
	ThemeXml xml;
	theme_xml_load(dir, &xml);
	snprintf(doc->origin_titles[0], sizeof(doc->origin_titles[0]), "%s", xml.title);
	snprintf(doc->origin_providers[0], sizeof(doc->origin_providers[0]), "%s", xml.provider);
	doc->origin_count = 1;
	return 0;
}

// --- themes made of pictures -------------------------------------------------------------

// The folder of a picture becomes a picture origin; *file is its name in it.
static int picture_origin(ThemeDoc *doc, const char *path, const char **file) {
	const char *slash = strrchr(path, '/');
	if (!slash || !slash[1])
		return -1;
	char dir[256];
	snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);
	*file = slash + 1;
	for (int i = 0; i < doc->origin_count; i++)
		if (doc->origin_pictures[i] && strcmp(doc->origins[i], dir) == 0)
			return i;
	if (doc->origin_count == THEMEDOC_MAX_ORIGINS)
		return -1;
	int i = doc->origin_count++;
	snprintf(doc->origins[i], sizeof(doc->origins[i]), "%s", dir);
	snprintf(doc->origin_titles[i], sizeof(doc->origin_titles[i]), "Your pictures");
	doc->origin_providers[i][0] = '\0'; // your own pictures aren't credited to anyone
	doc->origin_pictures[i] = 1;
	return i;
}

static XmlNode *add_child(XmlNode *parent, const char *name, const char *text) {
	XmlNode *n = xml_new(name, text);
	if (n)
		xml_append(parent, n);
	return n;
}

// A file reference to a picture, owned by the picture's folder origin.
static int add_picture_ref(ThemeDoc *doc, XmlNode *parent, const char *name, const char *picture) {
	const char *file;
	int origin = picture_origin(doc, picture, &file);
	XmlNode *n = origin >= 0 ? add_child(parent, name, file) : NULL;
	if (!n)
		return -1;
	n->origin = origin;
	return 0;
}

int themedoc_from_pictures(ThemeDoc *doc, const char *const *pictures, int count, const char *lock_picture) {
	memset(doc, 0, sizeof(*doc));
	if (count < 0 || count > THEMEDOC_MAX_PAGES)
		return -1;
	const char *lock = lock_picture && lock_picture[0] ? lock_picture : count > 0 ? pictures[0] : NULL;
	if (!lock)
		return -1;
	doc->root = xml_new("theme", NULL);
	if (!doc->root)
		return -1;
	free(doc->root->attrs);
	doc->root->attrs = malloc(40);
	if (doc->root->attrs)
		snprintf(doc->root->attrs, 40, " format-ver=\"01.00\" package=\"0\"");

	// Plain defaults that read well over any picture: white text, dark bars.
	XmlNode *start = add_child(doc->root, "StartScreenProperty", NULL);
	XmlNode *bar = add_child(doc->root, "InfomationBarProperty", NULL);
	XmlNode *info = add_child(doc->root, "InfomationProperty", NULL);
	XmlNode *home_node = add_child(doc->root, "HomeProperty", NULL);
	XmlNode *params = home_node ? add_child(home_node, "m_bgParam", NULL) : NULL;
	int ok = start && bar && info && params && add_picture_ref(doc, start, "m_filePath", lock) == 0;
	if (ok) {
		add_child(start, "m_dateColor", "ffffffff");
		add_child(start, "m_dateLayout", "1");
		add_child(start, "m_notifyBgColor", "b4000000");
		add_child(start, "m_notifyBorderColor", "ff505050");
		add_child(start, "m_notifyFontColor", "ffffffff");
		add_child(bar, "m_barColor", "ff000000");
		add_child(bar, "m_indicatorColor", "ffffffff");
		add_child(bar, "m_noticeFontColor", "ffffffff");
		add_child(bar, "m_noticeGlowColor", "ff000000");
		add_child(info, "m_contentVer", "01.00");
		XmlNode *title = add_child(info, "m_title", NULL), *provider = add_child(info, "m_provider", NULL);
		ok = title && provider && add_child(title, "m_default", "My pictures") &&
		     add_child(provider, "m_default", "Imago");
	}
	for (int i = 0; ok && i < count; i++) {
		XmlNode *page = add_child(params, "BackgroundParam", NULL);
		ok = page && add_picture_ref(doc, page, "m_thumbnailFilePath", pictures[i]) == 0 &&
		     add_picture_ref(doc, page, "m_imageFilePath", pictures[i]) == 0 &&
		     add_child(page, "m_fontColor", "ffffffff") && add_child(page, "m_fontShadow", "1");
	}
	if (!ok) {
		themedoc_free(doc);
		return -1;
	}
	return 0;
}

void themedoc_free(ThemeDoc *doc) {
	xml_free(doc->root);
	memset(doc, 0, sizeof(*doc));
}

int themedoc_copy(ThemeDoc *dst, const ThemeDoc *src) {
	*dst = *src;
	dst->root = xml_clone(src->root);
	return dst->root ? 0 : -1;
}

// --- title and provider ----------------------------------------------------------------

static XmlNode *ensure(XmlNode *parent, const char *name) {
	XmlNode *n = xml_find(parent, name);
	if (!n) {
		n = xml_new(name, NULL);
		xml_append(parent, n);
	}
	return n;
}

static void set_localized(ThemeDoc *doc, const char *field, const char *value) {
	XmlNode *info = ensure(doc->root, "InfomationProperty");
	XmlNode *block = ensure(info, field);
	xml_set_text(ensure(block, "m_default"), value);
	XmlNode *param = xml_find(block, "m_param");
	for (XmlNode *c = param ? param->child : NULL; c; c = c->next)
		xml_set_text(c, value);
}

void themedoc_get_title(const ThemeDoc *doc, char *out, size_t size) {
	xml_get_text(xml_find(doc->root, "InfomationProperty/m_title/m_default"), out, size);
}

void themedoc_set_title(ThemeDoc *doc, const char *title) {
	set_localized(doc, "m_title", title);
}

void themedoc_set_provider(ThemeDoc *doc, const char *provider) {
	set_localized(doc, "m_provider", provider);
}

// --- parts -------------------------------------------------------------------------

static int page_nodes(const ThemeDoc *doc, XmlNode **out, int max) {
	XmlNode *params = xml_find(home(doc), "m_bgParam");
	int n = 0;
	for (XmlNode *c = params ? params->child : NULL; c && n < max; c = c->next)
		if (strcmp(c->name, "BackgroundParam") == 0)
			out[n++] = c;
	return n;
}

int themedoc_page_count(const ThemeDoc *doc) {
	XmlNode *pages[THEMEDOC_MAX_PAGES];
	return page_nodes(doc, pages, THEMEDOC_MAX_PAGES);
}

int themedoc_icon_count(const ThemeDoc *doc) {
	int n = 0;
	XmlNode *h = home(doc);
	for (XmlNode *c = h ? h->child : NULL; c; c = c->next)
		if (is_icon_slot(c->name) && has_text(xml_find(c, "m_iconFilePath")))
			n++;
	return n;
}

static XmlNode *part_node(const ThemeDoc *doc, ThemePart part) {
	XmlNode *h = home(doc);
	switch (part) {
	case PART_WALLPAPERS: return xml_find(h, "m_bgParam");
	case PART_MUSIC: return xml_find(h, "m_bgmFilePath");
	case PART_LOCKSCREEN: return xml_find(doc->root, "StartScreenProperty");
	case PART_STATUSBAR: return xml_find(doc->root, "InfomationBarProperty");
	case PART_ICONS:
		for (XmlNode *c = h ? h->child : NULL; c; c = c->next)
			if (is_icon_slot(c->name) && has_text(xml_find(c, "m_iconFilePath")))
				return c;
		return NULL;
	default: return NULL;
	}
}

int themedoc_has_part(const ThemeDoc *doc, ThemePart part) {
	switch (part) {
	case PART_WALLPAPERS: return themedoc_page_count(doc) > 0;
	case PART_MUSIC: return has_text(part_node(doc, part));
	case PART_ICONS: return themedoc_icon_count(doc) > 0;
	case PART_LOCKSCREEN: return has_text(xml_find(part_node(doc, part), "m_filePath"));
	case PART_STATUSBAR: return part_node(doc, part) != NULL;
	default: return 0;
	}
}

int themedoc_part_origin(const ThemeDoc *doc, ThemePart part) {
	XmlNode *n = themedoc_has_part(doc, part) ? part_node(doc, part) : NULL;
	return n ? n->origin : -1;
}

int themedoc_remove_part(ThemeDoc *doc, ThemePart part) {
	XmlNode *h = home(doc);
	if (part == PART_MUSIC) {
		XmlNode *n = xml_find(h, "m_bgmFilePath");
		return n ? xml_remove(h, n) : 0;
	}
	if (part == PART_ICONS) {
		// Keep the slots but drop their files: the system icons come back.
		for (XmlNode *c = h ? h->child : NULL; c; c = c->next) {
			if (!is_icon_slot(c->name))
				continue;
			XmlNode *file;
			while ((file = xml_find(c, "m_iconFilePath")))
				xml_remove(c, file);
		}
		return 0;
	}
	return -1; // wallpapers, lock screen and status bar are required
}

static int add_origin(ThemeDoc *doc, const ThemeDoc *from, int index) {
	for (int i = 0; i < doc->origin_count; i++)
		if (strcmp(doc->origins[i], from->origins[index]) == 0 &&
		    doc->origin_pictures[i] == from->origin_pictures[index])
			return i;
	if (doc->origin_count == THEMEDOC_MAX_ORIGINS)
		return -1;
	int i = doc->origin_count++;
	snprintf(doc->origins[i], sizeof(doc->origins[i]), "%s", from->origins[index]);
	snprintf(doc->origin_titles[i], sizeof(doc->origin_titles[i]), "%s", from->origin_titles[index]);
	snprintf(doc->origin_providers[i], sizeof(doc->origin_providers[i]), "%s", from->origin_providers[index]);
	doc->origin_pictures[i] = from->origin_pictures[index];
	return i;
}

// Re-points a copied subtree's origins from `from`'s table to `doc`'s.
static int remap(XmlNode *n, ThemeDoc *doc, const ThemeDoc *from) {
	if (n->origin >= 0) {
		int o = add_origin(doc, from, n->origin);
		if (o < 0)
			return -1;
		n->origin = o;
	}
	for (XmlNode *c = n->child; c; c = c->next)
		if (remap(c, doc, from) != 0)
			return -1;
	return 0;
}

static XmlNode *copy_from(ThemeDoc *doc, const ThemeDoc *from, const XmlNode *src) {
	XmlNode *copy = xml_clone(src);
	if (copy && remap(copy, doc, from) != 0) {
		xml_free(copy);
		return NULL;
	}
	return copy;
}

static void prepend(XmlNode *parent, XmlNode *child) {
	free(parent->text);
	parent->text = NULL;
	child->next = parent->child;
	parent->child = child;
}

int themedoc_take_part(ThemeDoc *doc, ThemePart part, const ThemeDoc *from) {
	if (!themedoc_has_part(from, part))
		return -1;
	XmlNode *h = ensure(doc->root, "HomeProperty");
	XmlNode *src_home = home(from);
	if (part == PART_ICONS) {
		for (XmlNode *c = h->child, *next; c; c = next) {
			next = c->next;
			if (is_icon_slot(c->name))
				xml_remove(h, c);
		}
		for (XmlNode *c = src_home->child; c; c = c->next) {
			if (!is_icon_slot(c->name))
				continue;
			XmlNode *copy = copy_from(doc, from, c);
			if (!copy)
				return -1;
			xml_append(h, copy);
		}
		return 0;
	}
	const char *name = part == PART_WALLPAPERS  ? "m_bgParam"
	                   : part == PART_MUSIC     ? "m_bgmFilePath"
	                   : part == PART_LOCKSCREEN ? "StartScreenProperty"
	                                            : "InfomationBarProperty";
	XmlNode *parent = part == PART_WALLPAPERS || part == PART_MUSIC ? h : doc->root;
	XmlNode *src_parent = part == PART_WALLPAPERS || part == PART_MUSIC ? src_home : from->root;
	XmlNode *copy = copy_from(doc, from, xml_find(src_parent, name));
	if (!copy)
		return -1;
	XmlNode *existing = xml_find(parent, name);
	if (existing)
		xml_replace(parent, existing, copy);
	else if (part == PART_WALLPAPERS)
		prepend(parent, copy);
	else
		xml_append(parent, copy);
	return 0;
}

int themedoc_take_lockscreen_image(ThemeDoc *doc, const ThemeDoc *from) {
	XmlNode *src = xml_find(from->root, "StartScreenProperty/m_filePath");
	if (!has_text(src))
		return -1;
	XmlNode *start = xml_find(doc->root, "StartScreenProperty");
	if (!start)
		return themedoc_take_part(doc, PART_LOCKSCREEN, from);
	XmlNode *copy = copy_from(doc, from, src);
	if (!copy)
		return -1;
	XmlNode *existing = xml_find(start, "m_filePath");
	if (existing)
		xml_replace(start, existing, copy);
	else
		prepend(start, copy);
	return 0;
}

void themedoc_credit(const ThemeDoc *doc, char *out, size_t size) {
	int used[THEMEDOC_MAX_ORIGINS] = {0};
	used[0] = 1;
	for (int p = 0; p < PART_COUNT; p++) {
		int o = themedoc_part_origin(doc, (ThemePart)p);
		if (o >= 0)
			used[o] = 1;
	}
	out[0] = '\0';
	for (int i = 0; i < doc->origin_count; i++) {
		const char *who = doc->origin_providers[i];
		if (!used[i] || !who[0])
			continue;
		int seen = 0;
		for (int j = 0; j < i; j++)
			if (used[j] && strcmp(doc->origin_providers[j], who) == 0)
				seen = 1;
		if (seen)
			continue;
		size_t len = strlen(out);
		snprintf(out + len, size - len, "%s%s", len ? ", " : "", who);
	}
}

// --- files -------------------------------------------------------------------------

int themedoc_node_file(const ThemeDoc *doc, const XmlNode *node, char *out, size_t size) {
	char rel[256];
	xml_get_text(node, rel, sizeof(rel));
	for (char *p = rel; *p; p++)
		if (*p == '\\')
			*p = '/';
	// A theme may only reference files inside its own folder.
	if (!rel[0] || rel[0] == '/' || strchr(rel, ':') || strstr(rel, "..") || node->origin < 0 ||
	    node->origin >= doc->origin_count) {
		out[0] = '\0';
		return -1;
	}
	snprintf(out, size, "%s/%s", doc->origins[node->origin], rel);
	return 0;
}

int themedoc_page_file(const ThemeDoc *doc, int page, int thumbnail, char *out, size_t size) {
	XmlNode *pages[THEMEDOC_MAX_PAGES];
	int n = page_nodes(doc, pages, THEMEDOC_MAX_PAGES);
	out[0] = '\0';
	if (page < 0 || page >= n)
		return -1;
	XmlNode *file = xml_find(pages[page], thumbnail ? "m_thumbnailFilePath" : "m_imageFilePath");
	if (!has_text(file) && thumbnail)
		file = xml_find(pages[page], "m_imageFilePath");
	return has_text(file) ? themedoc_node_file(doc, file, out, size) : -1;
}

int themedoc_icon_files(const ThemeDoc *doc, char (*out)[512], int max) {
	int n = 0;
	XmlNode *h = home(doc);
	for (XmlNode *c = h ? h->child : NULL; c && n < max; c = c->next) {
		XmlNode *file = is_icon_slot(c->name) ? xml_find(c, "m_iconFilePath") : NULL;
		if (has_text(file) && themedoc_node_file(doc, file, out[n], sizeof(out[n])) == 0)
			n++;
	}
	return n;
}

int themedoc_set_pages(ThemeDoc *doc, const ThemeDoc *const *froms, const int *pages, int count) {
	if (count < 1 || count > THEMEDOC_MAX_PAGES)
		return -1;
	XmlNode *params = xml_new("m_bgParam", NULL);
	if (!params)
		return -1;
	for (int i = 0; i < count; i++) {
		XmlNode *src[THEMEDOC_MAX_PAGES];
		int n = page_nodes(froms[i], src, THEMEDOC_MAX_PAGES);
		XmlNode *copy = pages[i] >= 0 && pages[i] < n ? copy_from(doc, froms[i], src[pages[i]]) : NULL;
		if (!copy) {
			xml_free(params);
			return -1;
		}
		xml_append(params, copy);
	}
	XmlNode *h = ensure(doc->root, "HomeProperty");
	XmlNode *existing = xml_find(h, "m_bgParam");
	if (existing)
		xml_replace(h, existing, params);
	else
		prepend(h, params);
	return 0;
}

int themedoc_keep_pages(ThemeDoc *doc, const int *keep, int count) {
	XmlNode *pages[THEMEDOC_MAX_PAGES];
	int n = page_nodes(doc, pages, THEMEDOC_MAX_PAGES), kept = 0;
	for (int i = 0; i < n && i < count; i++)
		kept += keep[i] != 0;
	if (kept == 0)
		return -1;
	XmlNode *params = xml_find(home(doc), "m_bgParam");
	for (int i = 0; i < n && i < count; i++)
		if (!keep[i])
			xml_remove(params, pages[i]);
	return 0;
}

void themedoc_music_file(const ThemeDoc *doc, char *out, size_t size) {
	XmlNode *n = xml_find(home(doc), "m_bgmFilePath");
	if (!has_text(n) || themedoc_node_file(doc, n, out, size) != 0)
		out[0] = '\0';
}

void themedoc_lockscreen_file(const ThemeDoc *doc, char *out, size_t size) {
	XmlNode *n = xml_find(doc->root, "StartScreenProperty/m_filePath");
	if (!has_text(n) || themedoc_node_file(doc, n, out, size) != 0)
		out[0] = '\0';
}

// How a referenced file gets into the new theme.
enum { CONVERT_NONE, CONVERT_WALLPAPER, CONVERT_THUMBNAIL };

typedef struct {
	char src[512];
	char dst[96];
	int convert;
} FileMap;

typedef struct {
	XmlNode **nodes;
	int count, capacity;
} NodeList;

// File references end in "FilePath", except the lock screen's m_filePath.
static int is_file_ref(const char *name) {
	size_t n = strlen(name);
	return n >= 8 && strcasecmp(name + n - 8, "filepath") == 0;
}

static void collect_files(XmlNode *n, NodeList *list) {
	for (; n; n = n->next) {
		if (is_file_ref(n->name) && has_text(n)) {
			if (list->count == list->capacity) {
				int cap = list->capacity ? list->capacity * 2 : 64;
				XmlNode **grown = realloc(list->nodes, (size_t)cap * sizeof(XmlNode *));
				if (!grown)
					return;
				list->nodes = grown;
				list->capacity = cap;
			}
			list->nodes[list->count++] = n;
		}
		collect_files(n->child, list);
	}
}

// Icon slots whose file reference was dropped (missing or unsafe) lose the
// reference entirely, so the system icon shows instead of a broken one.
static void prune_empty_icons(XmlNode *root) {
	XmlNode *h = xml_find(root, "HomeProperty");
	for (XmlNode *c = h ? h->child : NULL; c; c = c->next) {
		if (!is_icon_slot(c->name))
			continue;
		XmlNode *file = xml_find(c, "m_iconFilePath");
		if (file && !has_text(file))
			xml_remove(c, file);
	}
}

static int name_taken(const FileMap *map, int count, const char *name) {
	if (strcmp(name, PREVIEW_HOME) == 0 || strcmp(name, PREVIEW_LOCK) == 0 || strcmp(name, PREVIEW_THUMB) == 0 ||
	    strcmp(name, "theme.xml") == 0)
		return 1;
	for (int i = 0; i < count; i++)
		if (strcmp(map[i].dst, name) == 0)
			return 1;
	return 0;
}

static void base_name(const char *path, char *out, size_t size) {
	const char *slash = strrchr(path, '/');
	const char *b = slash ? slash + 1 : path;
	size_t n = 0;
	for (; *b && n + 1 < size; b++) // keep names simple for the shell
		out[n++] = (*b == ' ' || *b == '\'' || *b == '&') ? '_' : *b;
	out[n] = '\0';
}

// A picture's name in the theme: "IMG 0001.jpg" -> "IMG_0001.png" or
// "IMG_0001_t.png" for its page thumbnail.
static void converted_name(const char *src, int convert, char *out, size_t size) {
	char name[96];
	base_name(src, name, sizeof(name));
	char *dot = strrchr(name, '.');
	if (dot)
		*dot = '\0';
	snprintf(out, size, "%.80s%s.png", name, convert == CONVERT_THUMBNAIL ? "_t" : "");
}

// Turns a picture into a theme image: scaled to cover the size, 8-bit palette.
static int convert_picture(const char *src, const char *dst, int convert) {
	int w = convert == CONVERT_THUMBNAIL ? THEME_PAGE_THUMB_W : THEME_WALLPAPER_W;
	int h = convert == CONVERT_THUMBNAIL ? THEME_PAGE_THUMB_H : THEME_WALLPAPER_H;
	uint8_t *pixels = NULL;
	int sw, sh;
	if (image_load_any(src, &pixels, &sw, &sh, w, h) != 0)
		return -1;
	uint8_t *scaled = image_resize_cover(pixels, sw, sh, w, h);
	free(pixels);
	int rc = scaled ? image_save_png_indexed(dst, scaled, w, h) : -1;
	free(scaled);
	return rc;
}

// Writes a resized copy of `src` as a preview; falls back to copying the
// base theme's existing preview if the image can't be decoded.
static int make_preview(const char *src, const char *fallback, const char *dest, int w, int h) {
	uint8_t *pixels = NULL;
	int sw, sh, rc = -1;
	if (src[0] && image_load_any(src, &pixels, &sw, &sh, w, h) == 0) {
		uint8_t *scaled = image_resize_cover(pixels, sw, sh, w, h);
		if (scaled)
			rc = image_save_png(dest, scaled, w, h);
		free(scaled);
		free(pixels);
	}
	if (rc != 0 && fallback[0])
		rc = fs_copy_file(fallback, dest);
	return rc;
}

static void set_err(char *err, size_t size, const char *msg) {
	if (err && size)
		snprintf(err, size, "%s", msg);
}

int themedoc_write(const ThemeDoc *doc, const char *dest_dir, int new_previews, ThemeDocProgress progress,
                   void *user, char *err, size_t err_size) {
	if (fs_exists(dest_dir)) {
		set_err(err, err_size, "The destination folder already exists.");
		return -1;
	}
	XmlNode *out = xml_clone(doc->root);
	if (!out) {
		set_err(err, err_size, "Out of memory.");
		return -1;
	}
	XmlNode *info = ensure(out, "InfomationProperty");
	char old_home[512] = "", old_lock[512] = "", old_thumb[512] = "";
	if (new_previews) {
		// The previews are generated, not copied: detach them from any origin.
		static const char *const fields[3] = {"m_homePreviewFilePath", "m_startPreviewFilePath",
		                                      "m_packageImageFilePath"};
		static const char *const names[3] = {PREVIEW_HOME, PREVIEW_LOCK, PREVIEW_THUMB};
		char *olds[3] = {old_home, old_lock, old_thumb};
		for (int i = 0; i < 3; i++) {
			XmlNode *n = ensure(info, fields[i]);
			if (has_text(n))
				themedoc_node_file(doc, n, olds[i], 512);
			xml_set_text(n, names[i]);
			n->origin = GENERATED_ORIGIN;
		}
	}

	NodeList files = {0};
	collect_files(out, &files);
	FileMap *map = calloc(files.count ? (size_t)files.count : 1, sizeof(FileMap));
	int mapped = 0, rc = 0;
	for (int i = 0; map && i < files.count; i++) {
		XmlNode *n = files.nodes[i];
		if (n->origin == GENERATED_ORIGIN)
			continue;
		char src[512];
		if (themedoc_node_file(doc, n, src, sizeof(src)) != 0) {
			xml_set_text(n, ""); // unsafe reference: drop it
			continue;
		}
		if (strcmp(n->name, "m_iconFilePath") == 0 && !fs_exists(src)) {
			xml_set_text(n, ""); // some published themes reference icons they don't ship
			continue;
		}
		int convert = !doc->origin_pictures[n->origin]             ? CONVERT_NONE
		              : strcmp(n->name, "m_thumbnailFilePath") == 0 ? CONVERT_THUMBNAIL
		                                                            : CONVERT_WALLPAPER;
		int found = -1;
		for (int m = 0; m < mapped; m++)
			if (strcmp(map[m].src, src) == 0 && map[m].convert == convert)
				found = m;
		if (found < 0) {
			char name[96], candidate[96];
			if (convert)
				converted_name(src, convert, name, sizeof(name));
			else
				base_name(src, name, sizeof(name));
			snprintf(candidate, sizeof(candidate), "%s", name);
			for (int k = 1; name_taken(map, mapped, candidate); k++)
				snprintf(candidate, sizeof(candidate), "o%d_%d_%s", n->origin, k, name);
			found = mapped++;
			snprintf(map[found].src, sizeof(map[found].src), "%s", src);
			snprintf(map[found].dst, sizeof(map[found].dst), "%s", candidate);
			map[found].convert = convert;
		}
		xml_set_text(n, map[found].dst);
	}
	if (!map) {
		set_err(err, err_size, "Out of memory.");
		rc = -1;
	}
	prune_empty_icons(out);

	int total = mapped + (new_previews ? 3 : 0), done = 0;
	if (rc == 0 && fs_mkdir_p(dest_dir) != 0) {
		set_err(err, err_size, "Could not create the theme folder.");
		rc = -1;
	}
	for (int m = 0; rc == 0 && m < mapped; m++) {
		char dst[640];
		snprintf(dst, sizeof(dst), "%s/%s", dest_dir, map[m].dst);
		if (map[m].convert) {
			if (convert_picture(map[m].src, dst, map[m].convert) != 0) {
				char msg[200];
				const char *slash = strrchr(map[m].src, '/');
				snprintf(msg, sizeof(msg), "Couldn't use the picture %s (only JPEG and PNG pictures work).",
				         slash ? slash + 1 : map[m].src);
				set_err(err, err_size, msg);
				rc = -1;
			}
		}
		// A file the source theme itself is missing stays missing; anything
		// else that fails to copy is a real error.
		else if (fs_exists(map[m].src) && fs_copy_file(map[m].src, dst) != 0) {
			set_err(err, err_size, "Could not copy the theme's files (is the memory card full?).");
			rc = -1;
		}
		if (progress && progress(user, ++done, total))
			rc = -2;
	}

	if (rc == 0 && new_previews) {
		char wallpaper[512], lock[512], path[640];
		themedoc_page_file(doc, 0, 0, wallpaper, sizeof(wallpaper));
		themedoc_lockscreen_file(doc, lock, sizeof(lock));
		snprintf(path, sizeof(path), "%s/" PREVIEW_HOME, dest_dir);
		make_preview(wallpaper, old_home, path, 480, 272);
		snprintf(path, sizeof(path), "%s/" PREVIEW_THUMB, dest_dir);
		make_preview(wallpaper, old_thumb, path, 226, 128);
		snprintf(path, sizeof(path), "%s/" PREVIEW_LOCK, dest_dir);
		make_preview(lock[0] ? lock : wallpaper, old_lock, path, 480, 272);
		done += 3;
		if (progress)
			progress(user, done, total);
	}

	if (rc == 0) {
		char *xml = xml_serialize(out), path[640];
		snprintf(path, sizeof(path), "%s/theme.xml", dest_dir);
		if (!xml || fs_write_file(path, xml, strlen(xml)) != 0) {
			set_err(err, err_size, "Could not write theme.xml.");
			rc = -1;
		}
		free(xml);
	}
	if (rc != 0)
		fs_rm_rf(dest_dir);
	free(map);
	free(files.nodes);
	xml_free(out);
	return rc;
}
