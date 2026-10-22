// Host-side tests for the tbl_theme writer against a mock app.db.
// Built with -DAPP_DB_PATH pointing at a scratch file.
#include "../src/themedb.h"
#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
			failures++;                                                                            \
		}                                                                                          \
	} while (0)

static void make_db(void) {
	remove(APP_DB_PATH);
	sqlite3 *db;
	CHECK(sqlite3_open(APP_DB_PATH, &db) == SQLITE_OK);
	CHECK(sqlite3_exec(db,
	                   "CREATE TABLE tbl_theme (id TEXT NOT NULL UNIQUE, type INTEGER NOT NULL,"
	                   " packageImageFilePath TEXT, homePreviewFilePath TEXT, startPreviewFilePath TEXT,"
	                   " contentVer INTEGER, size INTEGER, lastModifiedTime INTEGER NOT NULL,"
	                   " titleDefault TEXT, title_0 TEXT, providerDefault TEXT, provider_0 TEXT, titleId TEXT);"
	                   "INSERT INTO tbl_theme (id, type, lastModifiedTime, titleDefault)"
	                   " VALUES ('ux0:theme/PCSB00000-OFFICIAL', 100, 1, 'Official');",
	                   NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_close(db);
}

static void dump_row(const char *id) {
	sqlite3 *db;
	sqlite3_stmt *st;
	sqlite3_open(APP_DB_PATH, &db);
	sqlite3_prepare_v2(db, "SELECT * FROM tbl_theme WHERE id = ?1", -1, &st, NULL);
	sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
	if (sqlite3_step(st) == SQLITE_ROW) {
		for (int c = 0; c < sqlite3_column_count(st); c++) {
			const unsigned char *v = sqlite3_column_text(st, c);
			printf("  %s=%s\n", sqlite3_column_name(st, c), v ? (const char *)v : "NULL");
		}
		const unsigned char *title0 = sqlite3_column_text(st, 9);
		const unsigned char *title_id = sqlite3_column_text(st, 12);
		CHECK(title0 && strcmp((const char *)title0, "Link's Awakening") == 0);
		CHECK(title_id == NULL);
		CHECK(sqlite3_column_int(st, 1) == 100);
		CHECK(sqlite3_column_int(st, 5) == 100);
	} else {
		CHECK(!"row missing");
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
}

static int count_rows(void) {
	DbTheme *list;
	int count = -1;
	char err[THEMEDB_ERROR_SIZE];
	if (themedb_list(&list, &count, err) == 0)
		free(list);
	return count;
}

static void write_bytes(const char *path, const void *data, size_t size) {
	FILE *f = fopen(path, "wb");
	fwrite(data, 1, size, f);
	fclose(f);
}

int main(void) {
	char err[THEMEDB_ERROR_SIZE] = "";
	make_db();
	CHECK(themedb_writable());

	ThemeXml xml = {0};
	strcpy(xml.title, "Link's Awakening");
	strcpy(xml.provider, "O'Brien");
	strcpy(xml.content_ver, "01.00");
	strcpy(xml.package_image, "preview_thumbnail.png");
	const char *dir = "ux0:customtheme/Links_Awakening";

	CHECK(themedb_register(dir, &xml, 1234, err) == 0);
	CHECK(themedb_register(dir, &xml, 5678, err) == 0); // replacing must not duplicate
	if (failures)
		printf("error: %s\n", err);
	dump_row(dir);

	DbTheme *list;
	int count;
	CHECK(themedb_list(&list, &count, err) == 0);
	CHECK(count == 2);
	for (int i = 0; i < count; i++)
		printf("list: %s | %s | %s | %lld\n", list[i].id, list[i].title, list[i].provider,
		       (long long)list[i].size);
	CHECK(count == 2 && list[1].size == 5678 && strcmp(list[1].provider, "O'Brien") == 0);
	free(list);

	// Unregister reports how many rows it removed, and matches a trailing slash
	// stored by another manager.
	CHECK(themedb_unregister("ux0:customtheme/Missing", err) == 0);
	sqlite3 *db;
	sqlite3_open(APP_DB_PATH, &db);
	sqlite3_exec(db, "INSERT INTO tbl_theme (id, type, lastModifiedTime) VALUES ('ux0:customtheme/Slash/', 100, 0)",
	             NULL, NULL, NULL);
	sqlite3_close(db);
	CHECK(themedb_unregister("ux0:customtheme/Slash", err) == 1);
	CHECK(count_rows() == 2);

	// On the console the shell keeps titles and authors as UTF-16LE blobs.
	{
		// "PlayStation® 30th 🎮" and "Ashnoxious"
		static const unsigned char title16[] = {'P', 0, 'l', 0, 'a', 0, 'y', 0, 'S', 0, 't', 0, 'a', 0, 't', 0,
		                                        'i', 0, 'o', 0, 'n', 0, 0xAE, 0, ' ', 0, '3', 0, '0', 0, 't', 0,
		                                        'h', 0, ' ', 0, 0x3C, 0xD8, 0xAE, 0xDF};
		static const unsigned char author16[] = {'A', 0, 's', 0, 'h', 0, 'n', 0, 'o', 0, 'x', 0, 'i', 0, 'o', 0,
		                                         'u', 0, 's', 0};
		sqlite3_stmt *st;
		sqlite3_open(APP_DB_PATH, &db);
		sqlite3_prepare_v2(db,
		                   "INSERT INTO tbl_theme (id, type, lastModifiedTime, titleDefault, providerDefault) "
		                   "VALUES ('ux0:customtheme/Blob', 100, 0, ?1, ?2)",
		                   -1, &st, NULL);
		sqlite3_bind_blob(st, 1, title16, sizeof(title16), SQLITE_STATIC);
		sqlite3_bind_blob(st, 2, author16, sizeof(author16), SQLITE_STATIC);
		CHECK(sqlite3_step(st) == SQLITE_DONE);
		sqlite3_finalize(st);
		sqlite3_close(db);
		CHECK(themedb_list(&list, &count, err) == 0);
		int found = 0;
		for (int i = 0; i < count; i++)
			if (strcmp(list[i].id, "ux0:customtheme/Blob") == 0) {
				found = 1;
				printf("blob title: %s | %s\n", list[i].title, list[i].provider);
				CHECK(strcmp(list[i].title, "PlayStation\xC2\xAE 30th \xF0\x9F\x8E\xAE") == 0);
				CHECK(strcmp(list[i].provider, "Ashnoxious") == 0);
			}
		CHECK(found);
		free(list);
		CHECK(themedb_unregister("ux0:customtheme/Blob", err) == 1);
	}

	// A live rollback journal means the shell is mid-write: refuse to write.
	static const unsigned char magic[12] = {0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7, 0, 0, 0, 0};
	write_bytes(APP_DB_PATH "-journal", magic, sizeof(magic));
	CHECK(themedb_register(dir, &xml, 1, err) != 0);
	printf("journal guard: %s\n", err);
	static const unsigned char zeroed[12] = {0};
	write_bytes(APP_DB_PATH "-journal", zeroed, sizeof(zeroed)); // committed (PERSIST mode)
	CHECK(themedb_unregister("ux0:customtheme/Missing", err) == 0);
	remove(APP_DB_PATH "-journal");

	// Backups: a healthy round trip...
	char backup[512], damaged[512];
	snprintf(backup, sizeof(backup), "%s.bak", APP_DB_PATH);
	snprintf(damaged, sizeof(damaged), "%s.damaged", APP_DB_PATH);
	CHECK(themedb_check(APP_DB_PATH, err) == 0);
	CHECK(themedb_backup(backup, err) == 0);
	CHECK(themedb_unregister(dir, err) == 1);
	CHECK(count_rows() == 1);
	CHECK(themedb_restore(backup, err) == 0);
	CHECK(count_rows() == 2);
	CHECK(fopen(APP_DB_PATH ".imago-old", "rb") != NULL); // the replaced file is kept

	// ...and a damaged backup is refused without touching app.db.
	write_bytes(damaged, "SQLite format 3\0garbage garbage garbage", 40);
	CHECK(themedb_check(damaged, err) != 0);
	CHECK(themedb_restore(damaged, err) != 0);
	printf("damaged restore: %s\n", err);
	CHECK(count_rows() == 2);
	CHECK(themedb_check(APP_DB_PATH, err) == 0);

	printf(failures ? "%d FAILURES\n" : "all tests passed\n", failures);
	return failures != 0;
}
