#include "fs.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __vita__
#include <psp2/io/devctl.h>
#include <psp2/io/stat.h>
#endif

#ifdef _WIN32
#define MKDIR(p) mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0777)
#endif

int fs_exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0;
}

int fs_is_dir(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int fs_mkdir_p(const char *path) {
	char tmp[512];
	size_t len = strlen(path);
	if (len == 0 || len >= sizeof(tmp))
		return -1;
	memcpy(tmp, path, len + 1);
	if (tmp[len - 1] == '/')
		tmp[len - 1] = '\0';
	// Skip the device prefix ("ux0:") so we never try to mkdir it.
	char *p = strchr(tmp, ':');
	p = p ? p + 1 : tmp;
	if (*p == '/')
		p++;
	for (; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (!fs_is_dir(tmp) && MKDIR(tmp) != 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	}
	if (!fs_is_dir(tmp) && MKDIR(tmp) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

typedef struct {
	const char *base;
	int result;
	int64_t size;
} TreeWalk;

typedef struct {
	char (*names)[256];
	int count, capacity;
} NameList;

static int collect_name(const char *name, int is_dir, void *user) {
	(void)is_dir;
	NameList *l = user;
	if (l->count == l->capacity) {
		int cap = l->capacity ? l->capacity * 2 : 32;
		void *grown = realloc(l->names, (size_t)cap * sizeof(*l->names));
		if (!grown)
			return 1;
		l->names = grown;
		l->capacity = cap;
	}
	snprintf(l->names[l->count++], sizeof(l->names[0]), "%s", name);
	return 0;
}

int fs_rm_rf(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0)
		return 0;
	if (!S_ISDIR(st.st_mode))
		return remove(path) == 0 ? 0 : -1;
	// Collect first: deleting while iterating can skip entries on the Vita.
	NameList list = {0};
	fs_list_dir(path, collect_name, &list);
	int result = 0;
	for (int i = 0; i < list.count; i++) {
		char child[512];
		snprintf(child, sizeof(child), "%s/%s", path, list.names[i]);
		if (fs_rm_rf(child) != 0)
			result = -1;
	}
	free(list.names);
	if (rmdir(path) != 0)
		return -1;
	return result;
}

static int size_entry(const char *name, int is_dir, void *user) {
	TreeWalk *w = user;
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", w->base, name);
	if (is_dir) {
		w->size += fs_tree_size(path);
	} else {
		struct stat st;
		if (stat(path, &st) == 0)
			w->size += st.st_size;
	}
	return 0;
}

int64_t fs_tree_size(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0)
		return 0;
	if (!S_ISDIR(st.st_mode))
		return st.st_size;
	TreeWalk w = {path, 0, 0};
	fs_list_dir(path, size_entry, &w);
	return w.size;
}

int fs_read_file(const char *path, char **data, size_t *size) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0) {
		fclose(f);
		return -1;
	}
	char *buf = malloc((size_t)n + 1);
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return -1;
	}
	fclose(f);
	buf[n] = '\0';
	*data = buf;
	if (size)
		*size = (size_t)n;
	return 0;
}

int fs_write_file(const char *path, const void *data, size_t size) {
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	size_t written = fwrite(data, 1, size, f);
	if (fclose(f) != 0 || written != size) {
		remove(path);
		return -1;
	}
	return 0;
}

int fs_copy_file(const char *src, const char *dst) {
	FILE *in = fopen(src, "rb");
	if (!in)
		return -1;
	FILE *out = fopen(dst, "wb");
	if (!out) {
		fclose(in);
		return -1;
	}
	enum { CHUNK = 128 * 1024 };
	char *buf = malloc(CHUNK);
	int result = buf ? 0 : -1;
	size_t n;
	while (result == 0 && (n = fread(buf, 1, CHUNK, in)) > 0) {
		if (fwrite(buf, 1, n, out) != n)
			result = -1;
	}
	if (ferror(in))
		result = -1;
	free(buf);
	fclose(in);
	if (fclose(out) != 0)
		result = -1;
	if (result != 0)
		remove(dst);
	return result;
}

int fs_rename(const char *from, const char *to) {
	return rename(from, to) == 0 ? 0 : -1;
}

int64_t fs_free_space(const char *device) {
#ifdef __vita__
	SceIoDevInfo info;
	memset(&info, 0, sizeof(info));
	if (sceIoDevctl(device, 0x3001, NULL, 0, &info, sizeof(info)) < 0)
		return -1;
	return info.free_size;
#else
	(void)device;
	return -1;
#endif
}

int fs_list_dir(const char *path, FsDirCallback cb, void *user) {
	DIR *dir = opendir(path);
	if (!dir)
		return -1;
	struct dirent *e;
	while ((e = readdir(dir))) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
#ifdef __vita__
		// The entry carries its own stat: stat() itself fails in some places
		// (ux0:picture) that can still be listed.
		int is_dir = SCE_S_ISDIR(e->d_stat.st_mode);
#else
		char full[512];
		snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
		int is_dir = fs_is_dir(full);
#endif
		if (cb(e->d_name, is_dir, user))
			break;
	}
	closedir(dir);
	return 0;
}

void fs_format_size(char *out, size_t size, int64_t bytes) {
	if (bytes < 1024)
		snprintf(out, size, "%d B", (int)bytes);
	else if (bytes < 1024 * 1024)
		snprintf(out, size, "%.0f KB", bytes / 1024.0);
	else if (bytes < 1024LL * 1024 * 1024)
		snprintf(out, size, "%.1f MB", bytes / (1024.0 * 1024.0));
	else
		snprintf(out, size, "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
}
