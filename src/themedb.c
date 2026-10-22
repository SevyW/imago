#include "themedb.h"

#include "dbtext.h"

#include "fs.h"
#include "sqlite3.h"

#ifdef __vita__
#include <psp2/io/fcntl.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JOURNAL_PATH APP_DB_PATH "-journal"
#define STAGED_PATH APP_DB_PATH ".imago"
#define OLD_PATH APP_DB_PATH ".imago-old"

// sqlite is built without its own mutexes; serialise all access here.
static pthread_mutex_t db_lock = PTHREAD_MUTEX_INITIALIZER;

static void set_error(char *err, sqlite3 *db, const char *what) {
	if (err)
		snprintf(err, THEMEDB_ERROR_SIZE, "%s: %s", what, db ? sqlite3_errmsg(db) : "unavailable");
}

static void set_message(char *err, const char *message) {
	if (err)
		snprintf(err, THEMEDB_ERROR_SIZE, "%s", message);
}

// The shell writes app.db too. Imago's VFS has no locks, so if the shell is
// in the middle of a transaction its journal would look "hot" and SQLite
// would roll it back underneath the shell. A live rollback journal starts
// with this magic; a committed one is deleted, truncated or zeroed.
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

static sqlite3 *open_db(const char *path, int writable, char *err) {
	if (writable && journal_is_live()) {
		set_message(err, "The system is updating its theme list right now. Try again in a moment.");
		return NULL;
	}
	sqlite3 *db = NULL;
	int flags = writable ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY;
	if (sqlite3_open_v2(path, &db, flags, NULL) != SQLITE_OK) {
		set_error(err, db, "Could not open the theme database");
		sqlite3_close(db);
		return NULL;
	}
	return db;
}

int themedb_writable(void) {
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

static void copy_text(char *out, size_t size, const unsigned char *text) {
	snprintf(out, size, "%s", text ? (const char *)text : "");
}

int themedb_list(DbTheme **themes, int *count, char *err) {
	*themes = NULL;
	*count = 0;
	pthread_mutex_lock(&db_lock);
	sqlite3 *db = open_db(APP_DB_PATH, 0, err);
	if (!db) {
		pthread_mutex_unlock(&db_lock);
		return -1;
	}
	sqlite3_stmt *stmt = NULL;
	int rc = -1;
	if (sqlite3_prepare_v2(db, "SELECT * FROM tbl_theme", -1, &stmt, NULL) != SQLITE_OK) {
		set_error(err, db, "Could not read themes");
		goto done;
	}
	int capacity = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (*count == capacity) {
			capacity = capacity ? capacity * 2 : 16;
			DbTheme *grown = realloc(*themes, (size_t)capacity * sizeof(DbTheme));
			if (!grown)
				break;
			*themes = grown;
		}
		DbTheme *t = &(*themes)[*count];
		memset(t, 0, sizeof(*t));
		for (int c = 0; c < sqlite3_column_count(stmt); c++) {
			const char *name = sqlite3_column_name(stmt, c);
			// The shell keeps these as UTF-16 blobs on the console (dbtext.h).
			if (strcmp(name, "id") == 0)
				db_column_string(stmt, c, t->id, sizeof(t->id));
			else if (strcmp(name, "titleDefault") == 0)
				db_column_string(stmt, c, t->title, sizeof(t->title));
			else if (strcmp(name, "providerDefault") == 0)
				db_column_string(stmt, c, t->provider, sizeof(t->provider));
			else if (strcmp(name, "packageImageFilePath") == 0)
				db_column_string(stmt, c, t->package_image, sizeof(t->package_image));
			else if (strcmp(name, "size") == 0)
				t->size = sqlite3_column_int64(stmt, c);
		}
		if (t->id[0])
			(*count)++;
	}
	rc = 0;
done:
	sqlite3_finalize(stmt);
	sqlite3_close(db);
	pthread_mutex_unlock(&db_lock);
	return rc;
}

int themedb_pages(DbPage **pages, int *count, char *err) {
	*pages = NULL;
	*count = 0;
	pthread_mutex_lock(&db_lock);
	sqlite3 *db = open_db(APP_DB_PATH, 0, err);
	if (!db) {
		pthread_mutex_unlock(&db_lock);
		return -1;
	}
	sqlite3_stmt *stmt = NULL;
	int rc = -1;
	// Negative page numbers are the shell's own screens, not home pages.
	if (sqlite3_prepare_v2(db,
	                       "SELECT pageNo, themeFile FROM tbl_appinfo_page WHERE pageNo >= 0 AND themeFile IS NOT NULL "
	                       "ORDER BY pageNo",
	                       -1, &stmt, NULL) != SQLITE_OK) {
		set_error(err, db, "Could not read the home screen pages");
		goto done;
	}
	int capacity = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (*count == capacity) {
			capacity = capacity ? capacity * 2 : 16;
			DbPage *grown = realloc(*pages, (size_t)capacity * sizeof(DbPage));
			if (!grown)
				break;
			*pages = grown;
		}
		DbPage *p = &(*pages)[*count];
		p->page = sqlite3_column_int(stmt, 0) + 1;
		db_column_string(stmt, 1, p->file, sizeof(p->file));
		if (p->file[0])
			(*count)++;
	}
	rc = 0;
done:
	sqlite3_finalize(stmt);
	sqlite3_close(db);
	pthread_mutex_unlock(&db_lock);
	return rc;
}

static int exec(sqlite3 *db, const char *sql, char *err) {
	if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
		set_error(err, db, "Database write failed");
		return -1;
	}
	return 0;
}

// Other managers may store the folder with a trailing slash; match both.
// Returns the number of rows removed, or -1.
static int delete_rows(sqlite3 *db, const char *dir, char *err) {
	sqlite3_stmt *stmt = NULL;
	int removed = -1;
	if (sqlite3_prepare_v2(db, "DELETE FROM tbl_theme WHERE id = ?1 OR id = ?1 || '/'", -1, &stmt, NULL) ==
	    SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, dir, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_DONE)
			removed = sqlite3_changes(db);
	}
	if (removed < 0)
		set_error(err, db, "Could not remove the theme entry");
	sqlite3_finalize(stmt);
	return removed;
}

static int starts_with(const char *s, const char *prefix) {
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int ends_with(const char *s, const char *suffix) {
	size_t a = strlen(s), b = strlen(suffix);
	return a >= b && strcmp(s + a - b, suffix) == 0;
}

enum { COL_MAX = 64 };

typedef struct {
	char name[48];
	int notnull;
	int has_default;
} Column;

static int read_columns(sqlite3 *db, Column *cols) {
	sqlite3_stmt *stmt = NULL;
	int n = 0;
	if (sqlite3_prepare_v2(db, "PRAGMA table_info(tbl_theme)", -1, &stmt, NULL) != SQLITE_OK)
		return 0;
	while (n < COL_MAX && sqlite3_step(stmt) == SQLITE_ROW) {
		copy_text(cols[n].name, sizeof(cols[n].name), sqlite3_column_text(stmt, 1));
		cols[n].notnull = sqlite3_column_int(stmt, 3);
		cols[n].has_default = sqlite3_column_type(stmt, 4) != SQLITE_NULL;
		n++;
	}
	sqlite3_finalize(stmt);
	return n;
}

// Builds an INSERT that adapts to the firmware's tbl_theme schema: the core
// columns every installer writes, plus any localized title/provider columns.
static int insert_row(sqlite3 *db, const char *dir, const ThemeXml *xml, int64_t size, char *err) {
	Column cols[COL_MAX];
	int ncols = read_columns(db, cols);
	if (ncols == 0) {
		set_error(err, db, "tbl_theme not found");
		return -1;
	}

	char sql[2048] = "INSERT INTO tbl_theme (";
	char values[512] = ") VALUES (";
	int used[COL_MAX] = {0};
	int nused = 0;
	for (int i = 0; i < ncols; i++) {
		const char *c = cols[i].name;
		int known = strcmp(c, "id") == 0 || strcmp(c, "type") == 0 ||
		            strcmp(c, "packageImageFilePath") == 0 || strcmp(c, "homePreviewFilePath") == 0 ||
		            strcmp(c, "startPreviewFilePath") == 0 || strcmp(c, "contentVer") == 0 ||
		            strcmp(c, "size") == 0 ||
		            ((starts_with(c, "title") || starts_with(c, "provider")) && !ends_with(c, "Id"));
		if (!known && !(cols[i].notnull && !cols[i].has_default))
			continue;
		size_t len = strlen(sql);
		snprintf(sql + len, sizeof(sql) - len, "%s\"%s\"", nused ? "," : "", c);
		len = strlen(values);
		snprintf(values + len, sizeof(values) - len, "%s?%d", nused ? "," : "", nused + 1);
		used[nused++] = i;
	}
	size_t len = strlen(sql);
	snprintf(sql + len, sizeof(sql) - len, "%s)", values);

	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		set_error(err, db, "Could not prepare the theme entry");
		return -1;
	}
	const char *pkg = xml->package_image[0] ? xml->package_image : "preview_thumbnail.png";
	const char *home = xml->home_preview[0] ? xml->home_preview : "preview_page.png";
	const char *start = xml->start_preview[0] ? xml->start_preview : "preview_lockscreen.png";
	for (int p = 0; p < nused; p++) {
		const char *c = cols[used[p]].name;
		int slot = p + 1;
		if (strcmp(c, "id") == 0)
			sqlite3_bind_text(stmt, slot, dir, -1, SQLITE_TRANSIENT);
		else if (strcmp(c, "type") == 0)
			sqlite3_bind_int(stmt, slot, 100);
		else if (strcmp(c, "packageImageFilePath") == 0)
			sqlite3_bind_text(stmt, slot, pkg, -1, SQLITE_TRANSIENT);
		else if (strcmp(c, "homePreviewFilePath") == 0)
			sqlite3_bind_text(stmt, slot, home, -1, SQLITE_TRANSIENT);
		else if (strcmp(c, "startPreviewFilePath") == 0)
			sqlite3_bind_text(stmt, slot, start, -1, SQLITE_TRANSIENT);
		else if (strcmp(c, "contentVer") == 0)
			sqlite3_bind_int(stmt, slot, theme_content_version(xml->content_ver));
		else if (strcmp(c, "size") == 0)
			sqlite3_bind_int64(stmt, slot, size);
		else if (starts_with(c, "title"))
			sqlite3_bind_text(stmt, slot, xml->title, -1, SQLITE_TRANSIENT);
		else if (starts_with(c, "provider"))
			sqlite3_bind_text(stmt, slot, xml->provider, -1, SQLITE_TRANSIENT);
		else
			sqlite3_bind_int(stmt, slot, 0); // unknown NOT NULL column
	}
	int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
	if (rc != 0)
		set_error(err, db, "Could not add the theme entry");
	sqlite3_finalize(stmt);
	return rc;
}

int themedb_register(const char *dir, const ThemeXml *xml, int64_t size, char *err) {
	pthread_mutex_lock(&db_lock);
	sqlite3 *db = open_db(APP_DB_PATH, 1, err);
	int rc = -1;
	if (db && exec(db, "BEGIN", err) == 0) {
		rc = delete_rows(db, dir, err) >= 0 && insert_row(db, dir, xml, size, err) == 0 ? 0 : -1;
		if (exec(db, rc == 0 ? "COMMIT" : "ROLLBACK", err) != 0)
			rc = -1;
	}
	sqlite3_close(db);
	pthread_mutex_unlock(&db_lock);
	return rc;
}

int themedb_unregister(const char *dir, char *err) {
	pthread_mutex_lock(&db_lock);
	sqlite3 *db = open_db(APP_DB_PATH, 1, err);
	int removed = db ? delete_rows(db, dir, err) : -1;
	sqlite3_close(db);
	pthread_mutex_unlock(&db_lock);
	return removed;
}

static int check_locked(const char *path, char *err) {
	sqlite3 *db = open_db(path, 0, err);
	if (!db)
		return -1;
	sqlite3_stmt *stmt = NULL;
	int ok = sqlite3_prepare_v2(db, "PRAGMA quick_check", -1, &stmt, NULL) == SQLITE_OK &&
	         sqlite3_step(stmt) == SQLITE_ROW && strcmp((const char *)sqlite3_column_text(stmt, 0), "ok") == 0;
	sqlite3_finalize(stmt);
	if (ok) {
		// It must also be a shell database, not just any valid SQLite file.
		ok = sqlite3_prepare_v2(db, "SELECT 1 FROM tbl_theme LIMIT 1", -1, &stmt, NULL) == SQLITE_OK;
		sqlite3_finalize(stmt);
	}
	sqlite3_close(db);
	if (!ok)
		set_message(err, "The theme database failed its integrity check.");
	return ok ? 0 : -1;
}

int themedb_check(const char *path, char *err) {
	pthread_mutex_lock(&db_lock);
	int rc = check_locked(path, err);
	pthread_mutex_unlock(&db_lock);
	return rc;
}

int themedb_backup(const char *dest_path, char *err) {
	char temp[512];
	snprintf(temp, sizeof(temp), "%s.tmp", dest_path);
	pthread_mutex_lock(&db_lock);
	// Never let a damaged database overwrite a good backup.
	int rc = check_locked(APP_DB_PATH, err);
	if (rc == 0 && fs_copy_file(APP_DB_PATH, temp) != 0) {
		set_message(err, "Could not copy the theme database.");
		rc = -1;
	}
	if (rc == 0) {
		remove(dest_path);
		if (fs_rename(temp, dest_path) != 0) {
			set_message(err, "Could not save the backup.");
			rc = -1;
		}
	}
	remove(temp);
	pthread_mutex_unlock(&db_lock);
	return rc;
}

int themedb_restore(const char *src_path, char *err) {
	pthread_mutex_lock(&db_lock);
	int rc = -1;
	remove(STAGED_PATH);
	if (check_locked(src_path, err) != 0) {
		set_message(err, "That backup is damaged, so it was not restored.");
	} else if (fs_copy_file(src_path, STAGED_PATH) != 0 || check_locked(STAGED_PATH, err) != 0) {
		set_message(err, "Could not stage the backup on ur0: (is it full?). Nothing was changed.");
	} else {
		// Swap by renaming only: app.db is never truncated or deleted until the
		// restored copy is in place, and the old file is put back on failure.
		remove(OLD_PATH);
		if (fs_rename(APP_DB_PATH, OLD_PATH) != 0) {
			set_message(err, "The system is using its theme database. Restart the Vita and try again.");
		} else if (fs_rename(STAGED_PATH, APP_DB_PATH) != 0) {
			fs_rename(OLD_PATH, APP_DB_PATH);
			set_message(err, "Could not put the backup in place. Nothing was changed.");
		} else {
			remove(JOURNAL_PATH); // belongs to the database that was replaced
			rc = 0;
		}
	}
	remove(STAGED_PATH);
	pthread_mutex_unlock(&db_lock);
	return rc;
}
