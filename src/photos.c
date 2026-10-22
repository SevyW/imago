#include "photos.h"

#include "fs.h"
#include "log.h"
#include "themedoc.h"

#include <errno.h>
#include <psp2/apputil.h>
#include <psp2/photoexport.h>
#include <psp2/sysmodule.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

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

static void ux0_path(char *path, size_t size);

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
		char out[256];
		snprintf(out, sizeof(out), "%s", tab + 1);
		ux0_path(out, sizeof(out));
		add_entry(m, line, out);
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

// PhotoExport reports where it saved a picture as photo0:/ALL/..., another
// name for ux0:picture/ALL/...; Imago works with the ux0: name.
static void ux0_path(char *path, size_t size) {
	if (strncmp(path, "photo0:", 7) != 0)
		return;
	const char *rest = path + 7;
	while (*rest == '/')
		rest++;
	char fixed[256];
	snprintf(fixed, sizeof(fixed), "ux0:picture/%s", rest);
	snprintf(path, size, "%s", fixed);
}

static int export_picture(const char *src, const char *title, char *out, size_t out_size) {
	static char work[64 * 1024]; // PhotoExport's working memory; calls are serialised by the lock
	PhotoExportParam param;
	memset(&param, 0, sizeof(param));
	param.version = PHOTO_EXPORT_VERSION;
	// The SDK declares the titles as UTF-32, but PhotoExport reads UTF-8: on a
	// console, UTF-32 and UTF-16 titles came out as their first letter.
	param.photoTitle = (const SceWChar32 *)title;
	param.gameTitle = (const SceWChar32 *)"Imago";
	out[0] = '\0';
	int rc = scePhotoExportFromFile(src, &param, work, NULL, NULL, out, (SceSize)out_size);
	ux0_path(out, out_size);
	return rc;
}

int photos_share(const LibraryItem *themes, int count, char *err, size_t err_size) {
	pthread_mutex_lock(&lock);
	// Unloaded again at the end: while it's loaded the system can't take
	// screenshots.
	if (sceSysmoduleLoadModule(SCE_SYSMODULE_PHOTO_EXPORT) != 0) {
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
	sceSysmoduleUnloadModule(SCE_SYSMODULE_PHOTO_EXPORT);
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

// After Photos changes, the system shuts apps out of ux0:picture for a while
// ("Not owner"); the same files are always reachable through photo0:.
static int remove_picture(const char *path, int *mounted) {
	char target[256];
	snprintf(target, sizeof(target), "%s", path);
	if (remove(target) != 0) {
		if (errno != EPERM || strncmp(path, "ux0:picture/", 12) != 0)
			return -1;
		if (!*mounted) {
			int rc = sceAppUtilPhotoMount();
			if (rc < 0)
				log_write("photos: couldn't mount photo0: (0x%08X)", (unsigned)rc);
			*mounted = rc >= 0 ? 1 : -1;
		}
		snprintf(target, sizeof(target), "photo0:%s", path + 12);
		if (remove(target) != 0)
			return -1;
	}
	// PhotoExport makes a folder (ALL/xx) for every picture or two: drop it
	// once it's empty. rmdir refuses while other pictures are in it.
	char *slash = strrchr(target, '/');
	if (slash) {
		*slash = '\0';
		if (strstr(target, "ALL/"))
			rmdir(target);
	}
	return 0;
}

// Whether src is a file of the theme in folder dir.
static int of_theme(const char *src, const char *dir) {
	size_t n = strlen(dir);
	while (n && dir[n - 1] == '/')
		n--;
	return strncasecmp(src, dir, n) == 0 && src[n] == '/';
}

// Deletes the pictures of one theme (theme_dir), or all of them (NULL).
static int delete_pictures(const char *theme_dir) {
	pthread_mutex_lock(&lock);
	Manifest m, kept = {0}; // kept: other themes' pictures, and ones that couldn't be deleted
	load_manifest(&m);
	int removed = 0, missing = 0, failed = 0, mounted = 0;
	for (int i = 0; i < m.count; i++) {
		const char *out = m.items[i].out;
		if (theme_dir && !of_theme(m.items[i].src, theme_dir)) {
			add_entry(&kept, m.items[i].src, out);
			continue;
		}
		if (!out[0])
			continue;
		if (remove_picture(out, &mounted) == 0) {
			removed++;
		} else if (errno == ENOENT) {
			if (missing++ == 0)
				log_write("photos: %s is already gone", out);
		} else {
			if (failed++ == 0)
				log_write("photos: couldn't delete %s (%s)", out, strerror(errno));
			add_entry(&kept, m.items[i].src, out);
		}
	}
	if (mounted > 0)
		sceAppUtilPhotoUmount(); // a mounted photo0: keeps the system from taking screenshots
	if (missing > 1 || failed > 1)
		log_write("photos: %d already gone, %d couldn't be deleted", missing, failed);
	free(m.items);
	if (kept.count)
		save_manifest(&kept);
	else
		remove(PHOTOS_MANIFEST);
	free(kept.items);
	pthread_mutex_unlock(&lock);
	return removed;
}

int photos_unshare(void) {
	return delete_pictures(NULL);
}

int photos_unshare_theme(const char *theme_dir) {
	return delete_pictures(theme_dir);
}

int photos_theme_count(const char *theme_dir) {
	pthread_mutex_lock(&lock);
	Manifest m;
	load_manifest(&m);
	int n = 0;
	for (int i = 0; i < m.count; i++)
		n += m.items[i].out[0] && of_theme(m.items[i].src, theme_dir);
	free(m.items);
	pthread_mutex_unlock(&lock);
	return n;
}

int photos_theme_pages(const char *theme_dir, const DbPage *pages, int page_count, int *out, int max) {
	pthread_mutex_lock(&lock);
	Manifest m;
	load_manifest(&m);
	int n = 0;
	for (int p = 0; p < page_count && n < max; p++) {
		char file[256];
		snprintf(file, sizeof(file), "%s", pages[p].file);
		ux0_path(file, sizeof(file)); // the shell may name it photo0:/...
		for (int i = 0; i < m.count; i++)
			if (m.items[i].out[0] && of_theme(m.items[i].src, theme_dir) &&
			    strcasecmp(skip_device(m.items[i].out), skip_device(file)) == 0) {
				out[n++] = pages[p].page;
				break;
			}
	}
	free(m.items);
	pthread_mutex_unlock(&lock);
	return n;
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
