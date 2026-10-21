// Queue of long-running theme operations, processed one at a time on a
// dedicated thread so browsing stays responsive while themes install.
#pragma once

#include <stdint.h>

typedef enum {
	TASK_INSTALL,
	TASK_UNINSTALL,
	TASK_REMOVE_ENTRY,
	TASK_DELETE_FILES,
	TASK_REGISTER,
	TASK_BUILD,
} TaskKind;

typedef enum {
	TASK_QUEUED,
	TASK_DOWNLOADING,
	TASK_EXTRACTING,
	TASK_FINISHING,
	TASK_DONE,
	TASK_FAILED,
	TASK_CANCELED,
} TaskState;

typedef struct {
	int uid;
	TaskKind kind;
	TaskState state;
	float progress; // 0..1 across all stages
	long long bytes_done, bytes_total;
	uint32_t psvt_id;
	char hash[41];
	char title[128];
	char dir[256];
	char message[160]; // error text when TASK_FAILED
	void *doc;         // TASK_BUILD: the ThemeDoc to write (owned by the task)
	int new_previews;  // TASK_BUILD: regenerate the preview images
} Task;

void tasks_init(void);
void tasks_shutdown(void);

// Installs (or updates) a repository theme. Returns the task uid.
int tasks_install(uint32_t psvt_id, const char *hash, const char *title);
// Removes the tbl_theme row and, unless keep_files, the folder.
int tasks_uninstall(const char *dir, const char *title, int keep_files);
// Deletes a theme folder that is not in tbl_theme.
int tasks_delete_files(const char *dir, const char *title);
// Writes an edited theme (a malloc'd ThemeDoc, which the task takes over and
// frees) as a new theme folder and adds it to the system themes.
int tasks_build(void *doc, const char *title, int new_previews);
// Adds a theme folder that is on the memory card but not in tbl_theme.
int tasks_register(const char *dir, const char *title);
void tasks_cancel(int uid);

// Latest task for a repository theme; returns 0 if there is none.
int tasks_find(uint32_t psvt_id, Task *out);
// The running task, if any. Returns the number of queued + running tasks.
int tasks_active(Task *out);
// Pops the next finished task (for notifications). Returns 0 when empty.
int tasks_pop_finished(Task *out);
