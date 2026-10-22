// Settings: install location, database backup/restore, cache and power.
#include "app.h"
#include "config.h"
#include "dialog.h"
#include "fs.h"
#include "images.h"
#include "jobs.h"
#include "log.h"
#include "themedb.h"

#include <math.h>
#include <psp2/power.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define LIST_TOP (HEADER_H + 20.0f)
#define ROW_H 54.0f
#define ROW_GAP 6.0f
#define MAX_BACKUPS (1 + BACKUP_AUTO_COUNT)

enum { ITEM_PARTITION, ITEM_BACKUP, ITEM_RESTORE, ITEM_CACHE, ITEM_RESTART, ITEM_REPORT, ITEM_ABOUT, ITEM_COUNT };

static int selected;
static long long cache_size = -1;
static int cache_busy;

static int partition_available(int p) {
	return p == PARTITION_UX0 || fs_free_space(config_partition_device(p)) >= 0;
}

static void cycle_partition(int dir) {
	int p = g_config.partition;
	for (int i = 0; i < PARTITION_COUNT; i++) {
		p = (p + dir + PARTITION_COUNT) % PARTITION_COUNT;
		if (partition_available(p))
			break;
	}
	g_config.partition = p;
	config_save();
}

typedef struct {
	long long size;
	int clear;
} CacheJob;

static void cache_work(void *ctx, NetClient *net) {
	(void)net;
	CacheJob *job = ctx;
	if (job->clear)
		images_clear_disk_cache();
	job->size = images_disk_cache_size();
}

static void cache_done(void *ctx) {
	CacheJob *job = ctx;
	cache_size = job->size;
	if (job->clear)
		app_toast("Image cache cleared");
	cache_busy = 0;
	free(job);
}

static void run_cache_job(int clear) {
	if (cache_busy)
		return;
	CacheJob *job = calloc(1, sizeof(*job));
	if (!job)
		return;
	job->clear = clear;
	cache_busy = 1;
	jobs_submit(cache_work, cache_done, job);
}

static void on_restart(int choice) {
	if (choice == 0)
		scePowerRequestColdReset();
}

// Backups that exist on the memory card, newest automatic ones first.
static char backup_paths[MAX_BACKUPS][64];
static char backup_labels[MAX_BACKUPS][64];
static int backup_count, restore_choice;

static void add_backup(const char *path, const char *kind) {
	struct stat st;
	if (stat(path, &st) != 0)
		return;
	snprintf(backup_paths[backup_count], sizeof(backup_paths[0]), "%s", path);
	// Show the date only when the file system reports a believable one.
	char when[40] = "";
	struct tm *t = localtime(&st.st_mtime);
	if (t && t->tm_year + 1900 >= 2015)
		strftime(when, sizeof(when), " · %d %b %Y, %H:%M", t);
	snprintf(backup_labels[backup_count], sizeof(backup_labels[0]), "%s%s", kind, when);
	backup_count++;
}

static void find_backups(void) {
	static const char *const names[BACKUP_AUTO_COUNT] = {"Latest automatic backup", "Older automatic backup",
	                                                     "Oldest automatic backup"};
	backup_count = 0;
	add_backup(BACKUP_MANUAL, "Manual backup");
	for (int i = 1; i <= BACKUP_AUTO_COUNT; i++) {
		char path[64];
		snprintf(path, sizeof(path), BACKUP_AUTO_FORMAT, i);
		add_backup(path, names[i - 1]);
	}
}

static void on_restore(int choice) {
	if (choice != 0)
		return;
	char err[THEMEDB_ERROR_SIZE];
	if (themedb_restore(backup_paths[restore_choice], err) == 0)
		dialog_confirm("Backup restored", "Restart your PS Vita now so the system reloads its theme list.",
		               "Restart", 0, on_restart);
	else
		dialog_message("Restore failed", err);
}

static void on_pick_backup(int choice) {
	if (choice < 0)
		return;
	restore_choice = choice;
	char msg[256];
	snprintf(msg, sizeof(msg),
	         "This replaces the system theme list with: %s. Themes added since then will "
	         "disappear from Settings (their files stay on the memory card).",
	         backup_labels[choice]);
	dialog_confirm("Restore theme database?", msg, "Restore", 1, on_restore);
}

static void activate(int item) {
	char err[THEMEDB_ERROR_SIZE];
	switch (item) {
	case ITEM_PARTITION:
		cycle_partition(1);
		break;
	case ITEM_BACKUP:
		fs_mkdir_p(BACKUP_DIR);
		if (themedb_backup(BACKUP_MANUAL, err) == 0)
			app_toast("Theme database backed up");
		else
			dialog_message("Backup failed", g_db_writable ? err : "Enable \"Unsafe homebrew\" in HENkaku Settings first.");
		break;
	case ITEM_RESTORE:
		find_backups();
		if (backup_count == 0) {
			dialog_message("No backup yet", "Imago backs up the theme database automatically before its "
			                                "first change in each session, or you can back it up here.");
		} else {
			const char *labels[MAX_BACKUPS];
			for (int i = 0; i < backup_count; i++)
				labels[i] = backup_labels[i];
			dialog_menu("Restore which backup?", labels, backup_count, 0, on_pick_backup);
		}
		break;
	case ITEM_CACHE:
		run_cache_job(1);
		break;
	case ITEM_RESTART:
		dialog_confirm("Restart PS Vita?", "Use this if a newly installed theme doesn't show up in Settings.",
		               "Restart", 0, on_restart);
		break;
	case ITEM_REPORT:
		log_write("report: the user opened Report a problem");
		dialog_message("Report a problem",
		               "Open an issue at github.com/SevyW/imago/issues and say what you were doing when it went "
		               "wrong.\n\nPlease attach Imago's log: copy " LOG_PATH " (and " LOG_PREVIOUS_PATH
		               " if it's there) to your PC with VitaShell, over USB or FTP. It lists what Imago did, never "
		               "anything personal.");
		break;
	case ITEM_ABOUT:
		dialog_message("Imago " APP_VERSION,
		               "Created by " APP_AUTHOR ". github.com/SevyW/imago\n\n"
		               "A fast custom theme browser for PS Vita. Themes come from psvt.ovh, the free "
		               "repository run by Red Squirrel, author of Custom Themes Manager. The site is "
		               "funded by donations: please consider supporting it.");
		break;
	}
}

void settings_update(const Input *in) {
	if (cache_size < 0 && !cache_busy)
		run_cache_job(0);
	if ((in->repeat & SCE_CTRL_UP) && selected > 0)
		selected--;
	if ((in->repeat & SCE_CTRL_DOWN) && selected < ITEM_COUNT - 1)
		selected++;
	if (selected == ITEM_PARTITION && (in->repeat & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)))
		cycle_partition((in->repeat & SCE_CTRL_LEFT) ? -1 : 1);
	if (in->pressed & BTN_OK)
		activate(selected);
	if (in->tapped) {
		int i = (int)(((float)in->tap_y - LIST_TOP) / (ROW_H + ROW_GAP));
		if (in->tap_y >= LIST_TOP && i >= 0 && i < ITEM_COUNT) {
			selected = i;
			activate(i);
		}
	}
}

void settings_draw(void) {
	static const char *const titles[ITEM_COUNT] = {
		"Install location", "Back up theme database", "Restore theme database",
		"Clear image cache", "Restart PS Vita", "Report a problem", "About Imago",
	};
	char values[ITEM_COUNT][64] = {{0}};
	snprintf(values[ITEM_PARTITION], 64, "%scustomtheme", config_partition_device(g_config.partition));
	snprintf(values[ITEM_BACKUP], 64, "Saved to %s", BACKUP_MANUAL);
	snprintf(values[ITEM_RESTORE], 64, "Choose a manual or automatic backup");
	if (cache_busy || cache_size < 0)
		snprintf(values[ITEM_CACHE], 64, "Calculating...");
	else
		fs_format_size(values[ITEM_CACHE], 64, cache_size);
	snprintf(values[ITEM_RESTART], 64, "Reload the system theme list");
	snprintf(values[ITEM_REPORT], 64, "How to send us the log");
	snprintf(values[ITEM_ABOUT], 64, "Version " APP_VERSION " \xC2\xB7 by " APP_AUTHOR);

	for (int i = 0; i < ITEM_COUNT; i++) {
		float x = MARGIN, w = SCREEN_W - 2 * MARGIN;
		float y = LIST_TOP + (float)i * (ROW_H + ROW_GAP);
		int focused = i == selected;
		ui_rrect(x, y, w, ROW_H, 14.0f, focused ? C_SURFACE2 : C_SURFACE);
		ui_text(FONT_BOLD, x + 20.0f, y + 23.0f, C_TEXT, titles[i]);
		ui_text(FONT_SMALL, x + 20.0f, y + 42.0f, C_TEXT3, i == ITEM_PARTITION ? "Where new themes are saved" : values[i]);
		if (i == ITEM_PARTITION) {
			float right = x + w - 20.0f;
			int vw = ui_text_width(FONT_BOLD, values[i]);
			float mid = y + ROW_H * 0.5f;
			ui_text(FONT_BOLD, right - (float)vw - 22.0f, mid + 6.0f, C_ACCENT, values[i]);
			ui_line(right - 12.0f, mid - 5.0f, right - 6.0f, mid, 2.0f, C_TEXT3);
			ui_line(right - 6.0f, mid, right - 12.0f, mid + 5.0f, 2.0f, C_TEXT3);
			ui_line(right - (float)vw - 32.0f, mid - 5.0f, right - (float)vw - 38.0f, mid, 2.0f, C_TEXT3);
			ui_line(right - (float)vw - 38.0f, mid, right - (float)vw - 32.0f, mid + 5.0f, 2.0f, C_TEXT3);
		}
		if (focused)
			ui_rrect_outline(x - 4.0f, y - 4.0f, w + 8.0f, ROW_H + 8.0f, 18.0f, 2.5f,
			                 ui_alpha(C_ACCENT, 0.75f + 0.25f * sinf(ui_time() * 4.0f)));
	}

	Hint hints[] = {{ui_ok_glyph(), selected == ITEM_PARTITION ? "Change" : "Select"}};
	app_footer(hints, 1, NULL);
}
