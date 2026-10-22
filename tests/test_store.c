// Host-side tests for backups, applying, reapplying and restoring, against a
// fake appmeta folder. Built with -DAPPMETA_DIR, -DDATA_DIR and -DAPP_DB_PATH
// pointing at scratch locations.
#include "../src/appdb.h"
#include "../src/fs.h"
#include "sqlite3.h"
#include "../src/livearea.h"
#include "../src/store.h"

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

#define ID "PCSB00245"

static void write_text(const char *path, const char *text) {
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		fs_mkdir_p(dir);
	}
	CHECK(fs_write_file(path, text, strlen(text)) == 0);
}

static void meta(const char *rel, const char *text) {
	char path[512];
	store_appmeta_path(ID, rel, path, sizeof(path));
	write_text(path, text);
}

// 1 when the appmeta file holds exactly `text`.
static int meta_is(const char *rel, const char *text) {
	char path[512], *data;
	size_t size;
	store_appmeta_path(ID, rel, path, sizeof(path));
	if (fs_read_file(path, &data, &size) != 0)
		return 0;
	int same = size == strlen(text) && memcmp(data, text, size) == 0;
	free(data);
	return same;
}

static int data_is(const char *rel, const char *text) {
	char path[512], *data;
	size_t size;
	snprintf(path, sizeof(path), STORE_DIR "/" ID "/%s", rel);
	if (fs_read_file(path, &data, &size) != 0)
		return 0;
	int same = size == strlen(text) && memcmp(data, text, size) == 0;
	free(data);
	return same;
}

static void custom(ArtPart part, const char *text) {
	char path[512];
	store_custom_path(ID, part, path, sizeof(path));
	write_text(path, text);
}

static const char *TEMPLATE = "<livearea style=\"a1\" format-ver=\"01.00\"><livearea-background><image>bg0.png</image>"
                              "</livearea-background><gate><startup-image>startup.png</startup-image></gate></livearea>";

static void setup(void) {
	fs_rm_rf(APPMETA_DIR);
	fs_rm_rf(DATA_DIR);
	meta("icon0.png", "ICON-v1");
	meta("pic0.png", "PIC-v1");
	meta("livearea/contents/template.xml", TEMPLATE);
	meta("livearea/contents/bg0.png", "BG-v1");
	meta("livearea/contents/startup.png", "GATE-v1");
}

int main(void) {
	char err[STORE_ERROR_SIZE] = "";
	setup();

	CHECK(store_valid_id(ID) && store_valid_id("VITASHELL") && store_valid_id("SKGD3PL0Y"));
	CHECK(!store_valid_id("pcsb00245") && !store_valid_id("PCSB0024") && !store_valid_id("../../xyz"));
	CHECK(store_state(ID) == STORE_STOCK);

	// Apply an icon and a background: originals backed up, files swapped in.
	custom(ART_ICON, "ICON-custom");
	custom(ART_BG, "BG-custom");
	CHECK(store_state(ID) == STORE_DRIFTED); // customized but not in place yet
	CHECK(store_apply(ID, err) == 0);
	if (err[0])
		printf("  (%s)\n", err);
	CHECK(meta_is("icon0.png", "ICON-custom"));
	CHECK(meta_is("livearea/contents/bg0.png", "BG-custom"));
	CHECK(meta_is("livearea/contents/startup.png", "GATE-v1"));
	CHECK(data_is("original/icon0.png", "ICON-v1"));
	CHECK(data_is("original/pic0.png", "PIC-v1"));
	CHECK(data_is("original/livearea/contents/bg0.png", "BG-v1"));
	CHECK(store_state(ID) == STORE_APPLIED);
	{
		char path[512];
		store_appmeta_path(ID, "icon0.png.imago", path, sizeof(path));
		CHECK(!fs_exists(path)); // no temporary files left behind
		store_appmeta_path(ID, "icon0.png.imago-old", path, sizeof(path));
		CHECK(!fs_exists(path));
	}

	// A database rebuild puts the same originals back: reapply, no new backup.
	meta("icon0.png", "ICON-v1");
	meta("livearea/contents/bg0.png", "BG-v1");
	CHECK(store_state(ID) == STORE_DRIFTED);
	CHECK(store_reapply(ID, err) == 0);
	CHECK(meta_is("icon0.png", "ICON-custom") && meta_is("livearea/contents/bg0.png", "BG-custom"));
	CHECK(!fs_exists(STORE_DIR "/" ID "/history/1"));
	CHECK(store_state(ID) == STORE_APPLIED);

	// An app update brings new artwork: it becomes the original, the old one goes to history.
	meta("icon0.png", "ICON-v2");
	CHECK(store_state(ID) == STORE_DRIFTED);
	CHECK(store_reapply(ID, err) == 0);
	CHECK(meta_is("icon0.png", "ICON-custom"));
	CHECK(data_is("original/icon0.png", "ICON-v2"));
	CHECK(data_is("history/1/original/icon0.png", "ICON-v1"));
	// The background is still Imago's, so its original carries over unchanged.
	CHECK(data_is("original/livearea/contents/bg0.png", "BG-v1"));
	CHECK(store_state(ID) == STORE_APPLIED);

	// Clearing one part puts back the (new) original of that part only.
	CHECK(store_clear_part(ID, ART_ICON, err) == 0);
	CHECK(meta_is("icon0.png", "ICON-v2"));
	CHECK(meta_is("livearea/contents/bg0.png", "BG-custom"));
	CHECK(!store_has_custom(ID, ART_ICON));
	CHECK(store_state(ID) == STORE_APPLIED);

	CHECK(store_apply(ID, err) == 0); // applying twice changes nothing
	CHECK(store_state(ID) == STORE_APPLIED);

	// If the system replaced a file after Imago wrote it, restoring leaves it alone.
	custom(ART_GATE, "GATE-custom");
	CHECK(store_apply(ID, err) == 0);
	CHECK(meta_is("livearea/contents/startup.png", "GATE-custom"));
	meta("livearea/contents/startup.png", "GATE-v3");

	// Restore everything.
	CHECK(store_restore(ID, err) == 0);
	CHECK(meta_is("icon0.png", "ICON-v2"));
	CHECK(meta_is("livearea/contents/bg0.png", "BG-v1"));
	CHECK(meta_is("livearea/contents/startup.png", "GATE-v3"));
	CHECK(store_state(ID) == STORE_STOCK);
	CHECK(!fs_exists(STORE_DIR "/" ID "/custom"));
	CHECK(fs_exists(STORE_DIR "/" ID "/original/icon0.png")); // the backup stays

	// Failure modes: nothing is written without a backup.
	setup();
	custom(ART_ICON, "ICON-custom");
	fs_rm_rf(APPMETA_DIR "/" ID);
	err[0] = '\0';
	CHECK(store_apply(ID, err) != 0 && err[0]);
	CHECK(!fs_exists(APPMETA_DIR "/" ID "/icon0.png"));

	// A part the app doesn't have is refused before anything is written.
	setup();
	{
		char path[512];
		store_appmeta_path(ID, "livearea/contents/template.xml", path, sizeof(path));
		remove(path);
	}
	custom(ART_ICON, "ICON-custom");
	custom(ART_BG, "BG-custom");
	err[0] = '\0';
	CHECK(store_apply(ID, err) != 0 && err[0]);
	CHECK(meta_is("icon0.png", "ICON-v1"));

	// app.db wins over template.xml: the shell shows its own copies (" 23.png").
	setup();
	meta("livearea/contents/ 23.png", "BG-shell");
	meta("livearea/contents/en/startup.png", "GATE-shell");
	{
		remove(APP_DB_PATH);
		sqlite3 *db;
		CHECK(sqlite3_open(APP_DB_PATH, &db) == SQLITE_OK);
		CHECK(sqlite3_exec(db,
		                   "CREATE TABLE tbl_livearea (titleId TEXT, org_path TEXT, style TEXT,"
		                   " background_image TEXT, gate_startupImage TEXT);"
		                   "CREATE TABLE tbl_livearea_frame (titleId TEXT, frameId TEXT);"
		                   "INSERT INTO tbl_livearea VALUES ('" ID "', 'ur0:appmeta/" ID "/livearea/contents',"
		                   " 'a4', ' 23.png', 'en\\startup.png');"
		                   "INSERT INTO tbl_livearea VALUES ('NPXS10002', 'vs0:app/NPXS10002/sce_sys/livearea/contents',"
		                   " 'a1', 'bg.png', 'gate.png');"
		                   "INSERT INTO tbl_livearea_frame VALUES ('" ID "', 'frame1');",
		                   NULL, NULL, NULL) == SQLITE_OK);
		sqlite3_close(db);
		appdb_refresh();
		LiveArea la;
		CHECK(appdb_livearea(ID, &la) == 0);
		CHECK(strcmp(la.style, "a4") == 0 && la.frame_count == 1);
		CHECK(la.bg_count == 1 && strcmp(la.bg[0], " 23.png") == 0);
		CHECK(la.gate_count == 1 && strcmp(la.gate[0], "en/startup.png") == 0);
		CHECK(appdb_livearea("NPXS10002", &la) != 0); // lives on vs0:, read-only
	}
	custom(ART_BG, "BG-custom");
	custom(ART_GATE, "GATE-custom");
	CHECK(store_apply(ID, err) == 0);
	CHECK(meta_is("livearea/contents/ 23.png", "BG-custom"));
	CHECK(meta_is("livearea/contents/en/startup.png", "GATE-custom"));
	CHECK(meta_is("livearea/contents/bg0.png", "BG-v1")); // the template's file is not what the shell shows
	CHECK(store_restore(ID, err) == 0);
	CHECK(meta_is("livearea/contents/ 23.png", "BG-shell"));
	CHECK(meta_is("livearea/contents/en/startup.png", "GATE-shell"));
	remove(APP_DB_PATH);

	if (failures)
		printf("%d failure(s)\n", failures);
	else
		printf("all store tests passed\n");
	return failures ? 1 : 0;
}
