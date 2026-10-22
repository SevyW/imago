// An app's LiveArea layout. The shell keeps what it shows in app.db
// (tbl_livearea, see appdb.h); template.xml in livearea/contents is only the
// fallback for apps without that row.
#pragma once

#include <stddef.h>

#define LIVEAREA_MAX_FILES 8
#define LIVEAREA_NAME_SIZE 96

// Standard sizes. Existing files win when they say otherwise.
#define ICON_W 128
#define ICON_H 128
#define BG_W 840
#define BG_H 500
#define GATE_W 280
#define GATE_H 158

// Where the LiveArea sits on the 960x544 screen.
#define LIVEAREA_X 60
#define LIVEAREA_Y 44

typedef struct {
	char style[16]; // "a1".."a5", "psmobile", ...
	// Image files, relative to livearea/contents, always with '/' separators.
	// Templates can list several (one per language); app.db names one.
	char bg[LIVEAREA_MAX_FILES][LIVEAREA_NAME_SIZE];
	int bg_count;
	char gate[LIVEAREA_MAX_FILES][LIVEAREA_NAME_SIZE];
	int gate_count;
	int frame_count; // live frames drawn over the background
} LiveArea;

// Parses template.xml text. Returns 0 on success.
int livearea_parse(const char *xml, LiveArea *out);
int livearea_read(const char *template_path, LiveArea *out);

// Checks a file name from a template or app.db and copies it to out with '/'
// separators. Names that could leave livearea/contents are refused (-1).
// With `trim`, surrounding whitespace is dropped (templates pad names; app.db
// names can really start with a space, e.g. " 23.png").
int livearea_clean_name(const char *name, size_t len, int trim, char *out, size_t size);

// Top-left of the start gate inside the background, for styles whose
// geometry is known. Returns 0 when known.
int livearea_gate_origin(const char *style, int *x, int *y);
