#include "appdb.h"

#include "dbtext.h"

#include "fs.h"
#include "sqlite3.h"
#include "store.h"

#ifdef __vita__
#include <psp2/io/fcntl.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JOURNAL_PATH APP_DB_PATH "-journal"

// sqlite is built without its own mutexes; serialise all access here.
static pthread_mutex_t db_lock = PTHREAD_MUTEX_INITIALIZER;

int appdb_unsafe_enabled(void) {
#ifdef __vita__
	SceUID fd = sceIoOpen(APP_DB_PATH, SCE_O_RDWR, 0);
	if (fd < 0)
		return 0;
	sceIoClose(fd);
	return 1;
#else
	FILE *f = fopen(APP_DB_PATH, "r+b");
	if (f)
		fclose(f);
	return f != NULL;
#endif
}

// While the shell is in the middle of a transaction its rollback journal
// starts with this magic, and a copy taken then could be inconsistent.
static int journal_is_live(void) {
	static const unsigned char magic[8] = {0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7};
	FILE *f = fopen(JOURNAL_PATH, "rb");
	if (!f)
		return 0;
	unsigned char head[8] = {0};
	size_t n = fread(head, 1, sizeof(head), f);
	fclose(f);
	return n == sizeof(head) && memcmp(head, magic, sizeof(magic)) == 0;
}

static int has_columns(sqlite3 *db, const char *table) {
	char sql[128];
	snprintf(sql, sizeof(sql), "PRAGMA table_info(\"%s\")", table);
	sqlite3_stmt *stmt = NULL;
	int id = 0, title = 0;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *name = (const char *)sqlite3_column_text(stmt, 1);
			id |= name && strcmp(name, "titleId") == 0;
			title |= name && strcmp(name, "title") == 0;
		}
	}
	sqlite3_finalize(stmt);
	return id && title;
}

// tbl_appinfo_icon holds one row per bubble with its title. If a firmware
// names it differently, use the first table that has the same two columns.
static int find_table(sqlite3 *db, char *out, size_t size) {
	if (has_columns(db, "tbl_appinfo_icon")) {
		snprintf(out, size, "tbl_appinfo_icon");
		return 0;
	}
	sqlite3_stmt *stmt = NULL;
	int rc = -1;
	if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type = 'table'", -1, &stmt, NULL) == SQLITE_OK) {
		while (rc != 0 && sqlite3_step(stmt) == SQLITE_ROW) {
			const char *name = (const char *)sqlite3_column_text(stmt, 0);
			if (name && strlen(name) < size && has_columns(db, name)) {
				snprintf(out, size, "%s", name);
				rc = 0;
			}
		}
	}
	sqlite3_finalize(stmt);
	return rc;
}

static void clean_title(char *dst, size_t size, const char *src) {
	size_t n = 0;
	int space = 0;
	for (; *src && n + 1 < size; src++) {
		if (*src == '\n' || *src == '\r' || *src == '\t' || *src == ' ') {
			space = n > 0;
			continue;
		}
		if (space && n + 2 < size)
			dst[n++] = ' ';
		space = 0;
		dst[n++] = *src;
	}
	dst[n] = '\0';
}

static void refresh_locked(void) {
	// When the shell is writing, keep the last snapshot instead.
	if (journal_is_live())
		return;
	char temp[256];
	snprintf(temp, sizeof(temp), "%s.tmp", APP_DB_SNAPSHOT);
	fs_mkdir_p(DATA_DIR);
	if (fs_copy_file(APP_DB_PATH, temp) == 0) {
		remove(APP_DB_SNAPSHOT);
		fs_rename(temp, APP_DB_SNAPSHOT);
	}
	remove(temp);
}

void appdb_refresh(void) {
	pthread_mutex_lock(&db_lock);
	refresh_locked();
	pthread_mutex_unlock(&db_lock);
}

int appdb_titles(AppTitle **titles, int *count) {
	*titles = NULL;
	*count = 0;
	pthread_mutex_lock(&db_lock);
	refresh_locked();
	sqlite3 *db = NULL;
	int rc = -1;
	if (sqlite3_open_v2(APP_DB_SNAPSHOT, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
		goto done;
	char table[64], sql[160];
	if (find_table(db, table, sizeof(table)) != 0)
		goto done;
	snprintf(sql, sizeof(sql), "SELECT titleId, title FROM \"%s\" WHERE titleId IS NOT NULL", table);
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
		goto done;
	int capacity = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		char id[32], title[256];
		db_column_string(stmt, 0, id, sizeof(id));
		db_column_string(stmt, 1, title, sizeof(title));
		if (!id[0] || !title[0] || strlen(id) >= sizeof((*titles)->id))
			continue;
		if (*count == capacity) {
			capacity = capacity ? capacity * 2 : 64;
			AppTitle *grown = realloc(*titles, (size_t)capacity * sizeof(AppTitle));
			if (!grown)
				break;
			*titles = grown;
		}
		AppTitle *t = &(*titles)[*count];
		snprintf(t->id, sizeof(t->id), "%s", id);
		clean_title(t->title, sizeof(t->title), title);
		if (t->title[0])
			(*count)++;
	}
	sqlite3_finalize(stmt);
	rc = 0;
done:
	sqlite3_close(db);
	pthread_mutex_unlock(&db_lock);
	return rc;
}

static void text_column(sqlite3_stmt *stmt, int col, char *out, size_t size) {
	db_column_string(stmt, col, out, size); // UTF-16 blobs too (dbtext.h)
}

// Reads the app's tbl_livearea row. Returns 0 when there is a usable one,
// 1 when the LiveArea lives outside ur0:appmeta (not changeable), -1 if none.
static int livearea_row(const char *id, LiveArea *out) {
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	int rc = -1;
	if (sqlite3_open_v2(APP_DB_SNAPSHOT, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
		goto done;
	if (sqlite3_prepare_v2(db,
	                       "SELECT org_path, style, background_image, gate_startupImage FROM tbl_livearea "
	                       "WHERE titleId = ?1",
	                       -1, &stmt, NULL) != SQLITE_OK)
		goto done;
	sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) != SQLITE_ROW)
		goto done;
	char org[160], expected[160], name[LIVEAREA_NAME_SIZE * 2];
	text_column(stmt, 0, org, sizeof(org));
	snprintf(expected, sizeof(expected), "ur0:appmeta/%s/livearea/contents", id);
	if (strcmp(org, expected) != 0) {
		rc = 1;
		goto done;
	}
	text_column(stmt, 1, out->style, sizeof(out->style));
	text_column(stmt, 2, name, sizeof(name));
	if (livearea_clean_name(name, strlen(name), 0, out->bg[0], sizeof(out->bg[0])) == 0)
		out->bg_count = 1;
	text_column(stmt, 3, name, sizeof(name));
	if (livearea_clean_name(name, strlen(name), 0, out->gate[0], sizeof(out->gate[0])) == 0)
		out->gate_count = 1;
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM tbl_livearea_frame WHERE titleId = ?1", -1, &stmt, NULL) ==
	    SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW)
			out->frame_count = sqlite3_column_int(stmt, 0);
	}
	rc = 0;
done:
	sqlite3_finalize(stmt);
	sqlite3_close(db);
	return rc;
}

int appdb_livearea(const char *id, LiveArea *out) {
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&db_lock);
	int rc = livearea_row(id, out);
	pthread_mutex_unlock(&db_lock);
	if (rc == 1)
		return -1;
	if (rc == 0 && (out->bg_count || out->gate_count))
		return 0;
	char path[256];
	store_appmeta_path(id, "livearea/contents/template.xml", path, sizeof(path));
	return livearea_read(path, out);
}
