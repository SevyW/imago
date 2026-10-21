#include "tasks.h"

#include "config.h"
#include "fs.h"
#include "jobs.h"
#include "net.h"
#include "psvt.h"
#include "theme.h"
#include "themedb.h"
#include "themedoc.h"
#include "unzip.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/shellutil.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TASKS 48
#define DOWNLOAD_PATH DATA_DIR "/download.zip"
#define STAGING_SUFFIX ".imago-new"
#define OLD_SUFFIX ".imago-old"
#define MIN_DOWNLOAD_SPACE (32LL * 1024 * 1024)
#define SPACE_MARGIN (16LL * 1024 * 1024)

static Task tasks[MAX_TASKS];
static int canceled[MAX_TASKS];
static int finished_queue[MAX_TASKS], finished_head, finished_count;
static int next_uid = 1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_t thread;
static int thread_started, stopping;
static int backed_up; // app.db is copied once per session before the first write

// --- worker-side helpers (lock not held unless noted) -----------------------

static void update(int slot, TaskState state, float progress, long long done, long long total) {
	pthread_mutex_lock(&lock);
	tasks[slot].state = state;
	tasks[slot].progress = progress;
	tasks[slot].bytes_done = done;
	tasks[slot].bytes_total = total;
	pthread_mutex_unlock(&lock);
}

static int fail(int slot, const char *message) {
	pthread_mutex_lock(&lock);
	snprintf(tasks[slot].message, sizeof(tasks[slot].message), "%s", message);
	pthread_mutex_unlock(&lock);
	return -1;
}

static int is_canceled(int slot) {
	pthread_mutex_lock(&lock);
	int c = canceled[slot] || stopping;
	pthread_mutex_unlock(&lock);
	return c;
}

typedef struct {
	int slot;
	float from, span;
} ProgressCtx;

static int on_download(void *user, int64_t done, int64_t total) {
	ProgressCtx *p = user;
	sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
	float f = total > 0 ? (float)done / (float)total : 0.0f;
	update(p->slot, TASK_DOWNLOADING, p->from + p->span * f, done, total);
	return is_canceled(p->slot);
}

static int on_extract(void *user, uint64_t done, uint64_t total) {
	ProgressCtx *p = user;
	sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
	float f = total ? (float)done / (float)total : 0.0f;
	update(p->slot, TASK_EXTRACTING, p->from + p->span * f, (long long)done, (long long)total);
	return is_canceled(p->slot);
}

// Before the first change of a session: refuse to touch a damaged app.db,
// then rotate the automatic backups (auto-1 is the newest) and add a fresh one.
static int ensure_backup(int slot) {
	if (backed_up)
		return 0;
	char err[THEMEDB_ERROR_SIZE];
	if (themedb_check(APP_DB_PATH, err) != 0)
		return fail(slot, "The system theme list failed its integrity check, so Imago won't change it. "
		                  "Restore a backup in Settings.");
	fs_mkdir_p(BACKUP_DIR);
	char from[128], to[128];
	for (int i = BACKUP_AUTO_COUNT; i > 1; i--) {
		snprintf(from, sizeof(from), BACKUP_AUTO_FORMAT, i - 1);
		snprintf(to, sizeof(to), BACKUP_AUTO_FORMAT, i);
		if (fs_exists(from)) {
			remove(to);
			fs_rename(from, to);
		}
	}
	snprintf(to, sizeof(to), BACKUP_AUTO_FORMAT, 1);
	if (themedb_backup(to, err) != 0) {
		char message[200];
		snprintf(message, sizeof(message), "Couldn't back up the theme list, so nothing was changed. %s", err);
		return fail(slot, message);
	}
	backed_up = 1;
	return 0;
}

// Database writes happen with the PS button locked so the shell cannot
// suspend the app halfway through a transaction.
static int db_register(int slot, const char *dir, const ThemeXml *xml, long long size) {
	if (ensure_backup(slot) != 0)
		return -1;
	char err[THEMEDB_ERROR_SIZE];
	sceShellUtilLock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN);
	int rc = themedb_register(dir, xml, size, err);
	sceShellUtilUnlock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN);
	return rc == 0 ? 0 : fail(slot, err);
}

// Returns the number of rows removed, or -1.
static int db_unregister(int slot, const char *dir) {
	if (ensure_backup(slot) != 0)
		return -1;
	char err[THEMEDB_ERROR_SIZE];
	sceShellUtilLock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN);
	int removed = themedb_unregister(dir, err);
	sceShellUtilUnlock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN);
	if (removed < 0)
		fail(slot, err);
	return removed;
}

typedef struct {
	const char *base;
	uint32_t id;
	char found[256];
} MarkerSearch;

static int find_marker(const char *name, int is_dir, void *user) {
	MarkerSearch *s = user;
	if (!is_dir || strstr(name, STAGING_SUFFIX) || strstr(name, OLD_SUFFIX))
		return 0;
	char dir[256];
	ThemeMarker m;
	snprintf(dir, sizeof(dir), "%s/%s", s->base, name);
	if (theme_marker_read(dir, &m) == 0 && m.id == s->id) {
		snprintf(s->found, sizeof(s->found), "%s", dir);
		return 1;
	}
	return 0;
}

// Picks the folder for a repository theme: the folder of a previous install of
// the same theme (an update), else a title-based name that does not collide.
static void choose_folder(const char *base, uint32_t id, const char *title, char *out, size_t size) {
	MarkerSearch search = {base, id, ""};
	fs_list_dir(base, find_marker, &search);
	if (search.found[0]) {
		snprintf(out, size, "%s", search.found);
		return;
	}
	char name[64];
	theme_folder_name(name, sizeof(name), title);
	snprintf(out, size, "%s/%s", base, name);
	// Never reuse a folder that belongs to something else.
	for (int n = 0; fs_exists(out) && n < 100; n++) {
		if (n == 0)
			snprintf(out, size, "%s/%s_%u", base, name, (unsigned)id);
		else
			snprintf(out, size, "%s/%s_%u_%d", base, name, (unsigned)id, n);
	}
}

static int run_install(int slot, Task *t, NetClient *net) {
	if (!net)
		return fail(slot, "The network isn't available.");
	if (!themedb_writable())
		return fail(slot, "Can't write the theme database. Enable \"Unsafe homebrew\" in HENkaku settings.");
	long long download_space = fs_free_space("ux0:");
	if (download_space >= 0 && download_space < MIN_DOWNLOAD_SPACE)
		return fail(slot, "Not enough free space on ux0: to download the theme.");

	char url[256];
	psvt_download_url(url, sizeof(url), t->hash);
	fs_mkdir_p(DATA_DIR);
	ProgressCtx dl = {slot, 0.0f, 0.7f};
	int rc = net_download(net, url, DOWNLOAD_PATH, on_download, &dl);
	if (rc == NET_CANCELED)
		return -2;
	if (rc != NET_OK)
		return fail(slot, net_error(net));

	// Check the space the archive says it needs before writing anything.
	const char *device = config_partition_device(g_config.partition);
	uint64_t needed = 0;
	if (unzip_measure(DOWNLOAD_PATH, &needed) != UNZIP_OK) {
		remove(DOWNLOAD_PATH);
		return fail(slot, "The download isn't a valid theme archive.");
	}
	long long free_bytes = fs_free_space(device);
	if (free_bytes >= 0 && (uint64_t)free_bytes < needed + SPACE_MARGIN) {
		remove(DOWNLOAD_PATH);
		char message[128], size[24];
		fs_format_size(size, sizeof(size), (int64_t)needed);
		snprintf(message, sizeof(message), "Not enough free space on %s (this theme needs %s).", device, size);
		return fail(slot, message);
	}

	char base[32], final_dir[256], staging[300], old[300];
	snprintf(base, sizeof(base), "%scustomtheme", device);
	choose_folder(base, t->psvt_id, t->title, final_dir, sizeof(final_dir));
	if (!theme_dir_is_safe(final_dir)) {
		remove(DOWNLOAD_PATH);
		return fail(slot, "Couldn't pick a folder for this theme.");
	}
	snprintf(staging, sizeof(staging), "%s" STAGING_SUFFIX, final_dir);
	snprintf(old, sizeof(old), "%s" OLD_SUFFIX, final_dir);
	fs_rm_rf(staging);

	ProgressCtx ex = {slot, 0.7f, 0.25f};
	uint64_t written = 0;
	rc = unzip_extract(DOWNLOAD_PATH, staging, on_extract, &ex, &written);
	remove(DOWNLOAD_PATH);
	if (rc != UNZIP_OK) {
		fs_rm_rf(staging);
		return rc == UNZIP_ERR_CANCELED ? -2 : fail(slot, unzip_strerror(rc));
	}

	update(slot, TASK_FINISHING, 0.96f, 0, 0);
	ThemeXml xml;
	if (theme_xml_load(staging, &xml) != 0) {
		fs_rm_rf(staging);
		return fail(slot, "This download doesn't contain a valid theme.xml.");
	}
	ThemeMarker marker = {t->psvt_id, ""};
	snprintf(marker.hash, sizeof(marker.hash), "%s", t->hash);
	theme_marker_write(staging, &marker);

	// Swap by renaming, so a failure at any step leaves the previous version
	// (and its tbl_theme row) exactly as it was.
	fs_rm_rf(old);
	int had_old = fs_exists(final_dir);
	if (had_old && fs_rename(final_dir, old) != 0) {
		fs_rm_rf(staging);
		return fail(slot, "Couldn't move the previous version aside. Nothing was changed.");
	}
	if (fs_rename(staging, final_dir) != 0) {
		if (had_old)
			fs_rename(old, final_dir);
		fs_rm_rf(staging);
		return fail(slot, "Couldn't put the new files in place. Nothing was changed.");
	}
	pthread_mutex_lock(&lock);
	snprintf(t->dir, sizeof(t->dir), "%s", final_dir);
	snprintf(tasks[slot].dir, sizeof(tasks[slot].dir), "%s", final_dir);
	pthread_mutex_unlock(&lock);

	if (db_register(slot, final_dir, &xml, (long long)written) != 0) {
		fs_rm_rf(final_dir);
		if (had_old)
			fs_rename(old, final_dir);
		return -1;
	}
	if (had_old)
		fs_rm_rf(old);
	return 0;
}

static int run_uninstall(int slot, Task *t) {
	update(slot, TASK_FINISHING, 0.3f, 0, 0);
	int delete_files = t->kind != TASK_REMOVE_ENTRY;
	// Imago only ever deletes ordinary folders directly inside customtheme.
	if (delete_files && !theme_dir_is_safe(t->dir))
		return fail(slot, "This theme isn't in a customtheme folder, so Imago won't delete its files. "
		                  "Use \"Remove from system only\" instead.");
	if (t->kind != TASK_DELETE_FILES) {
		if (!themedb_writable())
			return fail(slot, "Can't write the theme database. Enable \"Unsafe homebrew\" in HENkaku settings.");
		int removed = db_unregister(slot, t->dir);
		if (removed < 0)
			return -1;
		if (removed == 0 && delete_files)
			return fail(slot, "This theme wasn't in the system list, so its files were left alone.");
	}
	if (delete_files && fs_rm_rf(t->dir) != 0)
		return fail(slot, "Some of the theme's files could not be deleted.");
	return 0;
}

static int on_build(void *user, int done, int total) {
	ProgressCtx *p = user;
	sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
	update(p->slot, TASK_EXTRACTING, p->from + p->span * (total ? (float)done / (float)total : 0.0f), done, total);
	return is_canceled(p->slot);
}

static int run_build(int slot, Task *t) {
	ThemeDoc *doc = t->doc;
	if (!doc)
		return fail(slot, "Nothing to save.");
	if (!themedb_writable())
		return fail(slot, "Can't write the theme database. Enable \"Unsafe homebrew\" in HENkaku settings.");
	const char *device = config_partition_device(g_config.partition);
	long long free_bytes = fs_free_space(device);
	if (free_bytes >= 0 && free_bytes < MIN_DOWNLOAD_SPACE)
		return fail(slot, "Not enough free space on the install partition.");

	// New themes always get a fresh folder; nothing existing is replaced.
	char base[32], name[64], final_dir[256], staging[300];
	snprintf(base, sizeof(base), "%scustomtheme", device);
	fs_mkdir_p(base);
	theme_folder_name(name, sizeof(name), t->title);
	snprintf(final_dir, sizeof(final_dir), "%s/%s", base, name);
	for (int n = 2; fs_exists(final_dir) && n < 1000; n++)
		snprintf(final_dir, sizeof(final_dir), "%s/%s_%d", base, name, n);
	if (!theme_dir_is_safe(final_dir) || fs_exists(final_dir))
		return fail(slot, "Couldn't pick a folder for the new theme.");
	snprintf(staging, sizeof(staging), "%s" STAGING_SUFFIX, final_dir);
	fs_rm_rf(staging);

	char err[160] = "";
	ProgressCtx pc = {slot, 0.0f, 0.9f};
	int rc = themedoc_write(doc, staging, t->new_previews, on_build, &pc, err, sizeof(err));
	if (rc == -2)
		return -2;
	if (rc != 0)
		return fail(slot, err[0] ? err : "Couldn't write the new theme.");

	update(slot, TASK_FINISHING, 0.95f, 0, 0);
	ThemeXml xml;
	if (theme_xml_load(staging, &xml) != 0 || fs_rename(staging, final_dir) != 0) {
		fs_rm_rf(staging);
		return fail(slot, "Couldn't finish the new theme's folder.");
	}
	pthread_mutex_lock(&lock);
	snprintf(tasks[slot].dir, sizeof(tasks[slot].dir), "%s", final_dir);
	pthread_mutex_unlock(&lock);
	if (db_register(slot, final_dir, &xml, fs_tree_size(final_dir)) != 0) {
		fs_rm_rf(final_dir); // never leave an unregistered half-made theme behind
		return -1;
	}
	return 0;
}

static int run_register(int slot, Task *t) {
	if (!themedb_writable())
		return fail(slot, "Can't write the theme database. Enable \"Unsafe homebrew\" in HENkaku settings.");
	update(slot, TASK_FINISHING, 0.3f, 0, 0);
	ThemeXml xml;
	if (theme_xml_load(t->dir, &xml) != 0)
		return fail(slot, "This folder doesn't contain a valid theme.xml.");
	return db_register(slot, t->dir, &xml, fs_tree_size(t->dir));
}

static int next_queued(void) {
	int best = -1;
	for (int i = 0; i < MAX_TASKS; i++)
		if (tasks[i].uid && tasks[i].state == TASK_QUEUED && (best < 0 || tasks[i].uid < tasks[best].uid))
			best = i;
	return best;
}

static void *worker(void *arg) {
	(void)arg;
	NetClient *net = net_client_new();
	pthread_mutex_lock(&lock);
	while (!stopping) {
		int slot = next_queued();
		if (slot < 0) {
			pthread_cond_wait(&wake, &lock);
			continue;
		}
		Task t = tasks[slot];
		tasks[slot].state = t.kind == TASK_INSTALL ? TASK_DOWNLOADING : TASK_FINISHING;
		pthread_mutex_unlock(&lock);

		int rc;
		switch (t.kind) {
		case TASK_INSTALL: rc = run_install(slot, &t, net); break;
		case TASK_REGISTER: rc = run_register(slot, &t); break;
		case TASK_BUILD: rc = run_build(slot, &t); break;
		default: rc = run_uninstall(slot, &t); break;
		}

		if (t.doc) { // the worker owns build documents once they're queued
			themedoc_free(t.doc);
			free(t.doc);
		}
		pthread_mutex_lock(&lock);
		tasks[slot].doc = NULL;
		tasks[slot].state = rc == 0 ? TASK_DONE : rc == -2 ? TASK_CANCELED : TASK_FAILED;
		tasks[slot].progress = 1.0f;
		if (finished_count < MAX_TASKS) {
			finished_queue[(finished_head + finished_count) % MAX_TASKS] = tasks[slot].uid;
			finished_count++;
		}
	}
	pthread_mutex_unlock(&lock);
	net_client_free(net);
	return NULL;
}

// --- public API (main thread) -----------------------------------------------

void tasks_init(void) {
	sceShellUtilInitEvents(0);
	fs_mkdir_p(DATA_DIR);
	thread_started = jobs_spawn_thread(worker, NULL, &thread) == 0;
}

void tasks_shutdown(void) {
	pthread_mutex_lock(&lock);
	stopping = 1;
	pthread_cond_broadcast(&wake);
	pthread_mutex_unlock(&lock);
	if (thread_started)
		pthread_join(thread, NULL);
}

static int is_running(TaskState s) {
	return s == TASK_QUEUED || s == TASK_DOWNLOADING || s == TASK_EXTRACTING || s == TASK_FINISHING;
}

// Reuses the oldest finished slot. Called with the lock held.
static int free_slot(void) {
	int best = -1;
	for (int i = 0; i < MAX_TASKS; i++) {
		if (!tasks[i].uid)
			return i;
		if (!is_running(tasks[i].state) && (best < 0 || tasks[i].uid < tasks[best].uid))
			best = i;
	}
	return best;
}

static int enqueue(TaskKind kind, uint32_t id, const char *hash, const char *title, const char *dir) {
	pthread_mutex_lock(&lock);
	for (int i = 0; kind == TASK_INSTALL && i < MAX_TASKS; i++) {
		if (tasks[i].uid && tasks[i].psvt_id == id && is_running(tasks[i].state)) {
			int uid = tasks[i].uid; // already queued or installing
			pthread_mutex_unlock(&lock);
			return uid;
		}
	}
	int slot = free_slot();
	if (slot < 0) {
		pthread_mutex_unlock(&lock);
		return 0;
	}
	Task *t = &tasks[slot];
	memset(t, 0, sizeof(*t));
	t->uid = next_uid++;
	t->kind = kind;
	t->state = TASK_QUEUED;
	t->psvt_id = id;
	snprintf(t->hash, sizeof(t->hash), "%s", hash ? hash : "");
	snprintf(t->title, sizeof(t->title), "%s", title ? title : "");
	snprintf(t->dir, sizeof(t->dir), "%s", dir ? dir : "");
	canceled[slot] = 0;
	pthread_cond_signal(&wake);
	int uid = t->uid;
	pthread_mutex_unlock(&lock);
	return uid;
}

int tasks_install(uint32_t psvt_id, const char *hash, const char *title) {
	return enqueue(TASK_INSTALL, psvt_id, hash, title, NULL);
}

int tasks_uninstall(const char *dir, const char *title, int keep_files) {
	return enqueue(keep_files ? TASK_REMOVE_ENTRY : TASK_UNINSTALL, 0, NULL, title, dir);
}

int tasks_build(void *doc, const char *title, int new_previews) {
	int uid = enqueue(TASK_BUILD, 0, NULL, title, NULL);
	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAX_TASKS; i++) {
		if (tasks[i].uid == uid) {
			tasks[i].doc = doc;
			tasks[i].new_previews = new_previews;
			doc = NULL;
		}
	}
	pthread_mutex_unlock(&lock);
	if (doc) { // couldn't queue it: don't leak the document
		themedoc_free(doc);
		free(doc);
	}
	return uid;
}

int tasks_delete_files(const char *dir, const char *title) {
	return enqueue(TASK_DELETE_FILES, 0, NULL, title, dir);
}

int tasks_register(const char *dir, const char *title) {
	return enqueue(TASK_REGISTER, 0, NULL, title, dir);
}

void tasks_cancel(int uid) {
	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAX_TASKS; i++) {
		if (tasks[i].uid != uid)
			continue;
		if (tasks[i].state == TASK_QUEUED) {
			tasks[i].state = TASK_CANCELED;
			if (tasks[i].doc) { // never reached the worker, so free it here
				themedoc_free(tasks[i].doc);
				free(tasks[i].doc);
				tasks[i].doc = NULL;
			}
		} else if (tasks[i].state == TASK_DOWNLOADING || tasks[i].state == TASK_EXTRACTING) {
			canceled[i] = 1; // the worker notices at the next progress callback
		}
	}
	pthread_mutex_unlock(&lock);
}

int tasks_find(uint32_t psvt_id, Task *out) {
	int found = 0;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAX_TASKS; i++) {
		if (tasks[i].uid && tasks[i].psvt_id == psvt_id && (!found || tasks[i].uid > out->uid)) {
			*out = tasks[i];
			found = 1;
		}
	}
	pthread_mutex_unlock(&lock);
	return found;
}

int tasks_active(Task *out) {
	int count = 0, running = 0;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAX_TASKS; i++) {
		if (!tasks[i].uid || !is_running(tasks[i].state))
			continue;
		count++;
		if (out && tasks[i].state != TASK_QUEUED) {
			*out = tasks[i];
			running = 1;
		}
	}
	if (out && !running && count) {
		for (int i = 0; i < MAX_TASKS; i++)
			if (tasks[i].uid && tasks[i].state == TASK_QUEUED)
				*out = tasks[i];
	}
	pthread_mutex_unlock(&lock);
	return count;
}

int tasks_pop_finished(Task *out) {
	int found = 0;
	pthread_mutex_lock(&lock);
	while (finished_count > 0 && !found) {
		int uid = finished_queue[finished_head];
		finished_head = (finished_head + 1) % MAX_TASKS;
		finished_count--;
		for (int i = 0; i < MAX_TASKS; i++) {
			if (tasks[i].uid == uid) {
				*out = tasks[i];
				found = 1;
				break;
			}
		}
	}
	pthread_mutex_unlock(&lock);
	return found;
}
