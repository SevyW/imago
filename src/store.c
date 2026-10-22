#include "store.h"

#include "appdb.h"
#include "fs.h"
#include "livearea.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_WROTE 24
#define REL_SIZE (LIVEAREA_NAME_SIZE + 32)
#define PATH_SIZE 512

static const char *const part_files[ART_COUNT] = {"icon.png", "bg.png", "gate.png"};

typedef struct {
	char rel[REL_SIZE]; // path inside the app's appmeta folder
	uint32_t crc;
} Record;

typedef struct {
	Record items[MAX_WROTE];
	int count;
} Records;

static void set_message(char *err, const char *message) {
	if (err)
		snprintf(err, STORE_ERROR_SIZE, "%s", message);
}

// --- CRC-32 (the zlib polynomial) -----------------------------------------------

static uint32_t crc_table[256];

static void crc_init(void) {
	if (crc_table[1])
		return;
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_table[i] = c;
	}
}

uint32_t store_crc32_file(const char *path, int *ok) {
	crc_init();
	*ok = 0;
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	unsigned char buf[16384];
	uint32_t c = 0xFFFFFFFFu;
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		for (size_t i = 0; i < n; i++)
			c = crc_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
	*ok = !ferror(f);
	fclose(f);
	return c ^ 0xFFFFFFFFu;
}

// --- paths ----------------------------------------------------------------------

int store_valid_id(const char *id) {
	if (strlen(id) != 9)
		return 0;
	for (int i = 0; i < 9; i++)
		if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9')))
			return 0;
	return 1;
}

void store_appmeta_path(const char *id, const char *rel, char *out, size_t size) {
	snprintf(out, size, APPMETA_DIR "/%s%s%s", id, rel[0] ? "/" : "", rel);
}

static void app_path(const char *id, const char *rel, char *out, size_t size) {
	snprintf(out, size, STORE_DIR "/%s%s%s", id, rel[0] ? "/" : "", rel);
}

void store_custom_path(const char *id, ArtPart part, char *out, size_t size) {
	char rel[64];
	snprintf(rel, sizeof(rel), "custom/%s", part_files[part]);
	app_path(id, rel, out, size);
}

int store_has_custom(const char *id, ArtPart part) {
	char path[PATH_SIZE];
	store_custom_path(id, part, path, sizeof(path));
	return fs_exists(path);
}

// --- record files ---------------------------------------------------------------

// Lines of "<prefix> <rel> <crc in hex>".
static void load_records(const char *path, const char *prefix, Records *out) {
	memset(out, 0, sizeof(*out));
	char *text;
	if (fs_read_file(path, &text, NULL) != 0)
		return;
	size_t plen = strlen(prefix);
	char *save = NULL; // strtok_r: callers run on several threads
	for (char *line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
		if (strncmp(line, prefix, plen) != 0 || line[plen] != ' ')
			continue;
		char *rel = line + plen + 1, *space = strrchr(rel, ' ');
		if (!space || space == rel || (size_t)(space - rel) >= REL_SIZE)
			continue;
		if (out->count == MAX_WROTE)
			break;
		Record *r = &out->items[out->count++];
		snprintf(r->rel, sizeof(r->rel), "%.*s", (int)(space - rel), rel);
		r->crc = (uint32_t)strtoul(space + 1, NULL, 16);
	}
	free(text);
}

static int save_records(const char *path, const char *prefix, const Records *records) {
	char *text = malloc((size_t)records->count * (REL_SIZE + 32) + 1);
	if (!text)
		return -1;
	size_t n = 0;
	text[0] = '\0';
	for (int i = 0; i < records->count; i++)
		n += (size_t)sprintf(text + n, "%s %s %08X\n", prefix, records->items[i].rel, (unsigned)records->items[i].crc);
	char temp[PATH_SIZE];
	snprintf(temp, sizeof(temp), "%s.tmp", path);
	int rc = fs_write_file(temp, text, n);
	free(text);
	if (rc == 0) {
		remove(path);
		rc = fs_rename(temp, path);
	}
	return rc;
}

static const Record *find_record(const Records *records, const char *rel) {
	for (int i = 0; i < records->count; i++)
		if (strcmp(records->items[i].rel, rel) == 0)
			return &records->items[i];
	return NULL;
}

// original.txt can list more files than MAX_WROTE (a whole appmeta folder),
// so look single entries up directly in the file.
static int original_crc(const char *id, const char *rel, uint32_t *crc) {
	char path[PATH_SIZE], *text;
	app_path(id, "original.txt", path, sizeof(path));
	if (fs_read_file(path, &text, NULL) != 0)
		return -1;
	int found = -1;
	size_t rlen = strlen(rel);
	char *save = NULL; // strtok_r: callers run on several threads
	for (char *line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
		if (strncmp(line, "file ", 5) == 0 && strncmp(line + 5, rel, rlen) == 0 && line[5 + rlen] == ' ') {
			*crc = (uint32_t)strtoul(line + 6 + rlen, NULL, 16);
			found = 0;
			break;
		}
	}
	free(text);
	return found;
}

// --- backup ---------------------------------------------------------------------

typedef struct {
	FILE *list;
	// When backing up again after an app update: the files Imago wrote, and
	// the previous original/ folder that holds the real versions of those.
	const Records *ours;
	const char *previous;
	int failed;
} Backup;

typedef struct {
	Backup *b;
	const char *src, *dst, *rel; // rel: the folder's path inside the backup
} CopyCtx;

static void copy_tree(Backup *b, const char *src, const char *dst, const char *rel);

static int copy_entry(const char *name, int is_dir, void *user) {
	CopyCtx *c = user;
	char s[PATH_SIZE], d[PATH_SIZE], r[PATH_SIZE], prev[PATH_SIZE];
	snprintf(s, sizeof(s), "%s/%s", c->src, name);
	snprintf(d, sizeof(d), "%s/%s", c->dst, name);
	snprintf(r, sizeof(r), "%s%s%s", c->rel, c->rel[0] ? "/" : "", name);
	if (is_dir) {
		copy_tree(c->b, s, d, r);
		return c->b->failed;
	}
	int ok;
	const char *from = s;
	const Record *mine = c->b->ours ? find_record(c->b->ours, r) : NULL;
	if (mine && store_crc32_file(s, &ok) == mine->crc && ok) {
		// Still Imago's file, so it isn't an original: keep the previous one.
		snprintf(prev, sizeof(prev), "%s/%s", c->b->previous, r);
		if (!fs_exists(prev))
			return 0; // Imago created it; there is no original
		from = prev;
	}
	if (fs_copy_file(from, d) != 0) {
		c->b->failed = 1;
		return 1;
	}
	uint32_t a = store_crc32_file(from, &ok), b = ok ? store_crc32_file(d, &ok) : 0;
	if (!ok || a != b) {
		c->b->failed = 1;
		return 1;
	}
	fprintf(c->b->list, "file %s %08X\n", r, (unsigned)a);
	return 0;
}

static void copy_tree(Backup *b, const char *src, const char *dst, const char *rel) {
	if (b->failed)
		return;
	if (fs_mkdir_p(dst) != 0) {
		b->failed = 1;
		return;
	}
	CopyCtx c = {b, src, dst, rel};
	if (fs_list_dir(src, copy_entry, &c) != 0)
		b->failed = 1;
}

// Copies the app's appmeta folder to original/, replacing any earlier backup.
static int take_backup(const char *id, const Records *ours, const char *previous, char *err) {
	char list_path[PATH_SIZE], temp_list[PATH_SIZE], dir[PATH_SIZE], temp_dir[PATH_SIZE], src[PATH_SIZE];
	app_path(id, "original.txt", list_path, sizeof(list_path));
	app_path(id, "original", dir, sizeof(dir));
	store_appmeta_path(id, "", src, sizeof(src));
	if (!fs_is_dir(src)) {
		set_message(err, "This app has no artwork folder to back up.");
		return -1;
	}
	app_path(id, "original.tmp", temp_dir, sizeof(temp_dir));
	app_path(id, "original.txt.tmp", temp_list, sizeof(temp_list));
	fs_rm_rf(temp_dir);
	char parent[PATH_SIZE];
	app_path(id, "", parent, sizeof(parent));
	fs_mkdir_p(parent);
	FILE *list = fopen(temp_list, "wb");
	if (!list) {
		set_message(err, "Could not write the backup (is the memory card full?).");
		return -1;
	}
	Backup b = {list, ours, previous, 0};
	copy_tree(&b, src, temp_dir, "");
	int failed = b.failed;
	if (fclose(list) != 0)
		failed = 1;
	if (!failed) {
		fs_rm_rf(dir);
		remove(list_path);
		failed = fs_rename(temp_dir, dir) != 0 || fs_rename(temp_list, list_path) != 0;
	}
	if (failed) {
		fs_rm_rf(temp_dir);
		remove(temp_list);
		set_message(err, "Could not back up the original artwork, so nothing was changed.");
		return -1;
	}
	return 0;
}

int store_backup(const char *id, char *err) {
	char list_path[PATH_SIZE], dir[PATH_SIZE];
	app_path(id, "original.txt", list_path, sizeof(list_path));
	app_path(id, "original", dir, sizeof(dir));
	if (fs_exists(list_path) && fs_is_dir(dir))
		return 0;
	return take_backup(id, NULL, NULL, err);
}

// --- writing into appmeta ------------------------------------------------------

// Replaces dst with src: copied next to dst first, checked, then swapped in by
// renaming, with the previous file put back if the swap fails.
static int put_file(const char *src, const char *dst, uint32_t *crc_out) {
	char temp[PATH_SIZE], old[PATH_SIZE];
	snprintf(temp, sizeof(temp), "%s.imago", dst);
	snprintf(old, sizeof(old), "%s.imago-old", dst);
	int ok;
	uint32_t want = store_crc32_file(src, &ok);
	if (!ok)
		return -1;
	remove(temp);
	if (fs_copy_file(src, temp) != 0) {
		remove(temp);
		return -1;
	}
	uint32_t got = store_crc32_file(temp, &ok);
	if (!ok || got != want) {
		remove(temp);
		return -1;
	}
	int had = fs_exists(dst);
	remove(old);
	if (had && fs_rename(dst, old) != 0) {
		remove(temp);
		return -1;
	}
	if (fs_rename(temp, dst) != 0) {
		if (had)
			fs_rename(old, dst);
		remove(temp);
		return -1;
	}
	remove(old);
	if (crc_out)
		*crc_out = want;
	return 0;
}

typedef struct {
	char src[PATH_SIZE];
	char rel[REL_SIZE];
} Placement;

// The appmeta files a part replaces, relative to the app's folder.
static int part_targets(const char *id, ArtPart part, char rels[][REL_SIZE], int max) {
	char path[PATH_SIZE];
	if (part == ART_ICON) {
		store_appmeta_path(id, "icon0.png", path, sizeof(path));
		if (!fs_exists(path))
			return 0;
		snprintf(rels[0], REL_SIZE, "icon0.png");
		return 1;
	}
	LiveArea la;
	if (appdb_livearea(id, &la) != 0)
		return 0;
	int n = part == ART_BG ? la.bg_count : la.gate_count;
	for (int i = 0; i < n && i < max; i++)
		snprintf(rels[i], REL_SIZE, "livearea/contents/%s", part == ART_BG ? la.bg[i] : la.gate[i]);
	return n < max ? n : max;
}

static int any_custom(const char *id) {
	for (int p = 0; p < ART_COUNT; p++)
		if (store_has_custom(id, (ArtPart)p))
			return 1;
	return 0;
}

int store_apply(const char *id, char *err) {
	if (!store_valid_id(id)) {
		set_message(err, "Unknown app.");
		return -1;
	}
	if (store_backup(id, err) != 0)
		return -1;

	// Work out every placement before writing anything.
	Placement places[MAX_WROTE];
	int count = 0;
	for (int p = 0; p < ART_COUNT; p++) {
		if (!store_has_custom(id, (ArtPart)p))
			continue;
		char src[PATH_SIZE];
		store_custom_path(id, (ArtPart)p, src, sizeof(src));
		char rels[LIVEAREA_MAX_FILES][REL_SIZE];
		int n = part_targets(id, (ArtPart)p, rels, LIVEAREA_MAX_FILES);
		if (n == 0) {
			set_message(err, p == ART_ICON ? "This app has no bubble icon to replace."
			                                : "This app has no LiveArea image to replace.");
			return -1;
		}
		for (int i = 0; i < n && count < MAX_WROTE; i++) {
			snprintf(places[count].src, sizeof(places[count].src), "%s", src);
			snprintf(places[count].rel, sizeof(places[count].rel), "%s", rels[i]);
			count++;
		}
	}

	char list_path[PATH_SIZE];
	app_path(id, "custom.txt", list_path, sizeof(list_path));
	Records wrote;
	load_records(list_path, "wrote", &wrote);
	int rc = 0;
	for (int i = 0; i < count; i++) {
		char dst[PATH_SIZE];
		uint32_t crc;
		store_appmeta_path(id, places[i].rel, dst, sizeof(dst));
		if (put_file(places[i].src, dst, &crc) != 0) {
			set_message(err, "Could not write to the system's artwork folder. Is unsafe homebrew enabled?");
			rc = -1;
			break;
		}
		Record *r = (Record *)find_record(&wrote, places[i].rel);
		if (!r && wrote.count < MAX_WROTE)
			r = &wrote.items[wrote.count++];
		if (r) {
			snprintf(r->rel, sizeof(r->rel), "%s", places[i].rel);
			r->crc = crc;
		}
	}
	if (save_records(list_path, "wrote", &wrote) != 0 && rc == 0) {
		set_message(err, "Could not save the list of changes.");
		rc = -1;
	}
	return rc;
}

StoreState store_state(const char *id) {
	char list_path[PATH_SIZE];
	app_path(id, "custom.txt", list_path, sizeof(list_path));
	Records wrote;
	load_records(list_path, "wrote", &wrote);
	if (!any_custom(id))
		return wrote.count ? STORE_DRIFTED : STORE_STOCK;
	if (wrote.count == 0)
		return STORE_DRIFTED; // customized but never put in place
	for (int i = 0; i < wrote.count; i++) {
		char path[PATH_SIZE];
		int ok;
		store_appmeta_path(id, wrote.items[i].rel, path, sizeof(path));
		if (store_crc32_file(path, &ok) != wrote.items[i].crc || !ok)
			return STORE_DRIFTED;
	}
	return STORE_APPLIED;
}

// Moves original/ and original.txt to history/<n>/ (or back again with
// `undo`). to_dir receives the retired original/ folder's new path.
static int retire_originals(const char *id, char *to_dir, size_t size, int undo) {
	char history[PATH_SIZE], dir[PATH_SIZE], list[PATH_SIZE], to_list[PATH_SIZE];
	app_path(id, "original", dir, sizeof(dir));
	app_path(id, "original.txt", list, sizeof(list));
	if (undo) {
		snprintf(history, sizeof(history), "%.*s", (int)(strlen(to_dir) - strlen("/original")), to_dir);
		snprintf(to_list, sizeof(to_list), "%s/original.txt", history);
		fs_rm_rf(dir);
		remove(list);
		return fs_rename(to_dir, dir) == 0 && fs_rename(to_list, list) == 0 ? 0 : -1;
	}
	for (int n = 1; n < 1000; n++) {
		char rel[64];
		snprintf(rel, sizeof(rel), "history/%d", n);
		app_path(id, rel, history, sizeof(history));
		if (!fs_exists(history))
			break;
	}
	if (fs_mkdir_p(history) != 0)
		return -1;
	snprintf(to_dir, size, "%s/original", history);
	snprintf(to_list, sizeof(to_list), "%s/original.txt", history);
	if (fs_rename(dir, to_dir) != 0)
		return -1;
	if (fs_rename(list, to_list) != 0) {
		fs_rename(to_dir, dir);
		return -1;
	}
	return 0;
}

int store_reapply(const char *id, char *err) {
	char list_path[PATH_SIZE];
	app_path(id, "custom.txt", list_path, sizeof(list_path));
	Records wrote;
	load_records(list_path, "wrote", &wrote);
	// Files that are neither Imago's nor the backed-up originals mean the
	// system brought new artwork: those become the originals from now on.
	int system_changed = 0;
	for (int i = 0; i < wrote.count && !system_changed; i++) {
		char path[PATH_SIZE];
		int ok;
		uint32_t orig;
		store_appmeta_path(id, wrote.items[i].rel, path, sizeof(path));
		uint32_t now = store_crc32_file(path, &ok);
		if (ok && now != wrote.items[i].crc && (original_crc(id, wrote.items[i].rel, &orig) != 0 || orig != now))
			system_changed = 1;
	}
	if (system_changed) {
		char retired[PATH_SIZE];
		if (retire_originals(id, retired, sizeof(retired), 0) != 0) {
			set_message(err, "Could not set the old backup aside, so nothing was changed.");
			return -1;
		}
		if (take_backup(id, &wrote, retired, err) != 0) {
			// Without original/, the next apply would back up Imago's own files.
			retire_originals(id, retired, sizeof(retired), 1);
			return -1;
		}
	}
	return store_apply(id, err);
}

// Puts one appmeta file back the way the original backup has it. A file the
// system has replaced since Imago wrote it is left alone: it is already stock.
static int restore_file(const char *id, const Record *r) {
	char path[PATH_SIZE], orig[PATH_SIZE], rel[PATH_SIZE];
	int ok;
	store_appmeta_path(id, r->rel, path, sizeof(path));
	uint32_t now = store_crc32_file(path, &ok);
	if (ok && now != r->crc)
		return 0;
	snprintf(rel, sizeof(rel), "original/%s", r->rel);
	app_path(id, rel, orig, sizeof(orig));
	if (fs_exists(orig))
		return put_file(orig, path, NULL);
	return ok ? remove(path) : 0; // Imago created it
}

static int restore_records(const char *id, Records *wrote, const char rels[][REL_SIZE], int nrels, char *err) {
	int rc = 0;
	for (int i = 0; i < wrote->count;) {
		int match = nrels < 0;
		for (int k = 0; k < nrels && !match; k++)
			match = strcmp(rels[k], wrote->items[i].rel) == 0;
		if (!match) {
			i++;
			continue;
		}
		if (restore_file(id, &wrote->items[i]) != 0) {
			set_message(err, "Could not put an original file back. Your backup is still safe.");
			rc = -1;
			i++;
			continue;
		}
		wrote->items[i] = wrote->items[--wrote->count];
	}
	return rc;
}

int store_clear_part(const char *id, ArtPart part, char *err) {
	char list_path[PATH_SIZE], custom[PATH_SIZE];
	app_path(id, "custom.txt", list_path, sizeof(list_path));
	Records wrote;
	load_records(list_path, "wrote", &wrote);
	char rels[LIVEAREA_MAX_FILES][REL_SIZE];
	int n = part_targets(id, part, rels, LIVEAREA_MAX_FILES);
	int rc = restore_records(id, &wrote, (const char(*)[REL_SIZE])rels, n, err);
	if (rc == 0) {
		store_custom_path(id, part, custom, sizeof(custom));
		remove(custom);
	}
	if (save_records(list_path, "wrote", &wrote) != 0 && rc == 0) {
		set_message(err, "Could not save the list of changes.");
		rc = -1;
	}
	return rc;
}

int store_restore(const char *id, char *err) {
	char list_path[PATH_SIZE], custom[PATH_SIZE];
	app_path(id, "custom.txt", list_path, sizeof(list_path));
	Records wrote;
	load_records(list_path, "wrote", &wrote);
	int rc = restore_records(id, &wrote, NULL, -1, err);
	if (rc != 0) {
		save_records(list_path, "wrote", &wrote);
		return rc;
	}
	app_path(id, "custom", custom, sizeof(custom));
	fs_rm_rf(custom);
	remove(list_path);
	return 0;
}
