// Read-only access to the shell's app database (ur0:shell/db/app.db), for
// bubble titles. Imago never writes app.db: it reads a snapshot copy, so it
// can't disturb the shell or roll back a transaction the shell has in flight.
#pragma once

#include <stddef.h>

#include "config.h"
#include "livearea.h"

#ifndef APP_DB_PATH // overridden by host-side tests
#define APP_DB_PATH "ur0:shell/db/app.db"
#endif
#define APP_DB_SNAPSHOT DATA_DIR "/app-snapshot.db"

typedef struct {
	char id[16];
	char title[128];
} AppTitle;

// 1 when unsafe homebrew is enabled (the system folders are writable).
int appdb_unsafe_enabled(void);
// Copies app.db to the snapshot, unless the shell is writing it right now.
void appdb_refresh(void);
// Reads every bubble's title (from a fresh snapshot). *titles must be freed by the caller.
int appdb_titles(AppTitle **titles, int *count);
// The LiveArea the shell really shows for an app: its tbl_livearea row in the
// last snapshot (layout, background and gate files; these can differ from
// template.xml, e.g. " 23.png"), else the app's template.xml. Apps whose
// LiveArea lives outside ur0:appmeta/<id> (system apps on vs0:) are refused.
// Returns 0 when found.
int appdb_livearea(const char *id, LiveArea *out);
