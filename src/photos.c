#include "photos.h"

#include "fs.h"
#include "log.h"
#include "themedoc.h"

#include <psp2/photoexport.h>
#include <psp2/sysmodule.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PHOTO_EXPORT_VERSION 0x03150021
#define ERROR_ALREADY_EXPORTED 0x80101A0B

// The manifest is one "source<TAB>copy" line per picture added: the theme
// file and where PhotoExport put it.
typedef struct {
	char src[512];
	char out[256];
} Entry;

typedef struct {
	Entry *items;
	int count, capacity;
} Manifest;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int add_entry(Manifest *m, const char *src, const char *out) {
	if (m->count == m->capacity) {
		int cap = m->capacity ? m->capacity * 2 : 64;
		Entry *grown = realloc(m->items, (size_t)cap * sizeof(Entry));
		if (!grown)
			return -1;
		m->items = grown;
		m->capacity = cap;
	}
	snprintf(m->items[m->count].src, sizeof(m->items[0].src), "%s", src);
	snprintf(m->items[m->count].out, sizeof(m->items[0].out), "%s", out);
	m->count++;
	return 0;
}

static void load_manifest(Manifest *m) {
	memset(m, 0, sizeof(*m));
	char *text;
	if (fs_read_file(PHOTOS_MANIFEST, &text, NULL) != 0)
		return;
	for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
		char *tab = strchr(line, '\t');
		if (!tab)
			continue;
		*tab = '\0';
		add_entry(m, line, tab + 1);
	}
	free(text);
}

static void save_manifest(const Manifest *m) {
	size_t cap = 1 + (size_t)m->count * (sizeof(Entry) + 2), len = 0;
	char *text = malloc(cap);
	if (!text)
		return;
	for (int i = 0; i < m->count; i++)
		len += (size_t)snprintf(text + len, cap - len, "%s\t%s\n", m->items[i].src, m->items[i].out);
	fs_write_file(PHOTOS_MANIFEST, text, len);
	free(text);
}

static int shared(const Manifest *m, const char *src) {
	for (int i = 0; i < m->count; i++)
		if (strcmp(m->items[i].src, src) == 0)
			return 1;
	return 0;
}

// "ux0:/picture/x" and "ux0:picture/x" name the same file.
static const char *skip_device(const char *path) {
	const char *colon = strchr(path, ':');
	path = colon ? colon + 1 : path;
	while (*path == '/')
		path++;
	return path;
}

// PhotoExport takes UTF-32 titles.
static void to_utf32(const char *in, SceWChar32 *out, size_t size) {
	size_t n = 0;
	const unsigned char *s = (const unsigned char *)in;
	while (*s && n + 1 < size) {
		unsigned c = *s++;
		int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
		if (extra)
			c &= 0x3F >> extra;
		for (int i = 0; i < extra && (*s & 0xC0) == 0x80; i++)
			c = (c << 6) | (*s++ & 0x3F);
		out[n++] = c;
	}
	out[n] = 0;
}

static int export_picture(const char *src, const char *title, char *out, size_t out_size) {
	static char work[64 * 1024]; // PhotoExport's working memory; calls are serialised by the lock
	SceWChar32 title32[128], app32[8];
	to_utf32(title, title32, sizeof(title32) / sizeof(title32[0]));
	to_utf32("Imago", app32, sizeof(app32) / sizeof(app32[0]));
	PhotoExportParam param;
	memset(&param, 0, sizeof(param));
	param.version = PHOTO_EXPORT_VERSION;
	param.photoTitle = title32;
	param.gameTitle = app32;
	out[0] = '\0';
	return scePhotoExportFromFile(src, &param, work, NULL, NULL, out, (SceSize)out_size);
}

int photos_share(const LibraryItem *themes, int count, char *err, size_t err_size) {
	pthread_mutex_lock(&lock);
	if (sceSysmoduleIsLoaded(SCE_SYSMODULE_PHOTO_EXPORT) != 0 &&
	    sceSysmoduleLoadModule(SCE_SYSMODULE_PHOTO_EXPORT) != 0) {
		snprintf(err, err_size, "The system's photo library isn't available.");
		pthread_mutex_unlock(&lock);
		return -1;
	}
	Manifest m;
	load_manifest(&m);
	int added = 0, failed = 0, last_error = 0;
	for (int t = 0; t < count; t++) {
		if (!themes[t].has_files)
			continue;
		ThemeDoc doc;
		if (themedoc_load(&doc, themes[t].dir) != 0)
			continue;
		char files[THEMEDOC_MAX_PAGES + 1][512], titles[THEMEDOC_MAX_PAGES + 1][200];
		int n = 0, pages = themedoc_page_count(&doc);
		for (int p = 0; p < pages; p++)
			if (themedoc_page_file(&doc, p, 0, files[n], sizeof(files[0])) == 0) {
				snprintf(titles[n], sizeof(titles[0]), "%s \xC2\xB7 page %d", themes[t].title, p + 1);
				n++;
			}
		themedoc_lockscreen_file(&doc, files[n], sizeof(files[0]));
		if (files[n][0]) {
			snprintf(titles[n], sizeof(titles[0]), "%s \xC2\xB7 lock screen", themes[t].title);
			n++;
		}
		themedoc_free(&doc);
		for (int i = 0; i < n; i++) {
			if (shared(&m, files[i]) || !fs_exists(files[i]))
				continue;
			char out[256];
			int rc = export_picture(files[i], titles[i], out, sizeof(out));
			if (rc < 0 && (unsigned)rc != ERROR_ALREADY_EXPORTED) {
				failed++;
				last_error = rc;
				continue;
			}
			if (add_entry(&m, files[i], rc < 0 ? "" : out) == 0 && rc >= 0)
				added++;
		}
	}
	save_manifest(&m);
	free(m.items);
	pthread_mutex_unlock(&lock);
	if (failed)
		log_write("photos: %d picture%s couldn't be added (0x%08X)", failed, failed == 1 ? "" : "s",
		          (unsigned)last_error);
	if (failed && !added) {
		snprintf(err, err_size, "The pictures couldn't be added to Photos (error 0x%08X).", (unsigned)last_error);
		return -1;
	}
	return added;
}

int photos_unshare(void) {
	pthread_mutex_lock(&lock);
	Manifest m;
	load_manifest(&m);
	int removed = 0;
	for (int i = 0; i < m.count; i++)
		if (m.items[i].out[0] && fs_exists(m.items[i].out) && remove(m.items[i].out) == 0)
			removed++;
	free(m.items);
	remove(PHOTOS_MANIFEST);
	pthread_mutex_unlock(&lock);
	return removed;
}

int photos_shared_count(void) {
	pthread_mutex_lock(&lock);
	Manifest m;
	load_manifest(&m);
	int n = 0;
	for (int i = 0; i < m.count; i++)
		n += m.items[i].out[0] != '\0';
	free(m.items);
	pthread_mutex_unlock(&lock);
	return n;
}

int photos_is_shared_copy(const char *path) {
	static Manifest cached; // reloaded when the manifest changes size (it only grows or goes)
	static int64_t cached_size = -1;
	pthread_mutex_lock(&lock);
	int64_t size = fs_tree_size(PHOTOS_MANIFEST);
	if (size != cached_size) {
		free(cached.items);
		load_manifest(&cached);
		cached_size = size;
	}
	int found = 0;
	const char *want = skip_device(path);
	for (int i = 0; i < cached.count && !found; i++)
		found = cached.items[i].out[0] && strcasecmp(skip_device(cached.items[i].out), want) == 0;
	pthread_mutex_unlock(&lock);
	return found;
}
