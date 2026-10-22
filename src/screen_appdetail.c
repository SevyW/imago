// One app: its current bubble and LiveArea, and what can be changed.
#include "app.h"
#include "appdb.h"
#include "dialog.h"
#include "fs.h"
#include "images.h"
#include "livearea.h"
#include "picture_picker.h"
#include "store.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PREVIEW_X 452.0f
#define PREVIEW_Y 96.0f
#define PREVIEW_W 480.0f
#define PREVIEW_H (PREVIEW_W * BG_H / BG_W)
#define ROWS_Y 214.0f
#define ROW_H 50.0f
#define ROW_GAP 6.0f
#define ROWS_W (PREVIEW_X - MARGIN - 24.0f)

enum { ROW_ICON, ROW_BG, ROW_GATE, ROW_REAPPLY, ROW_RESTORE };

static int is_open, closing;
static float appear;
static char app_id[16], title[128];
static LiveArea la;
static StoreState state;
static int rows[8], row_count, focus;

static int any_custom(void) {
	for (int p = 0; p < ART_COUNT; p++)
		if (store_has_custom(app_id, (ArtPart)p))
			return 1;
	return 0;
}

static void reload(void) {
	appdb_livearea(app_id, &la);
	state = store_state(app_id);
	const AppItem *it = apps_find(&g_apps, app_id);
	snprintf(title, sizeof(title), "%s", it ? it->title : app_id);

	row_count = 0;
	rows[row_count++] = ROW_ICON;
	if (la.bg_count)
		rows[row_count++] = ROW_BG;
	if (la.gate_count)
		rows[row_count++] = ROW_GATE;
	if (state == STORE_DRIFTED)
		rows[row_count++] = ROW_REAPPLY;
	if (any_custom() || state == STORE_DRIFTED)
		rows[row_count++] = ROW_RESTORE;
	if (focus >= row_count)
		focus = row_count - 1;
}

void appdetail_open(const char *id) {
	snprintf(app_id, sizeof(app_id), "%s", id);
	is_open = 1;
	closing = 0;
	appear = 0.0f;
	focus = 0;
	reload();
}

int appdetail_is_open(void) {
	return is_open;
}

// --- background jobs ------------------------------------------------------------

typedef enum { OP_CLEAR, OP_REAPPLY, OP_RESTORE } Op;

typedef struct {
	Op op;
	char id[16];
	ArtPart part;
	int icon; // the bubble icon changes too
	int rc;
	char err[STORE_ERROR_SIZE];
} Job;

static void job_work(void *ctx, NetClient *net) {
	(void)net;
	Job *j = ctx;
	switch (j->op) {
	case OP_CLEAR:
		j->rc = store_clear_part(j->id, j->part, j->err);
		if (j->rc == 0 && j->part == ART_BG) {
			// A gate cut from this background no longer matches anything.
			char marker[256];
			snprintf(marker, sizeof(marker), STORE_DIR "/%s/custom/gate-from-bg", j->id);
			if (remove(marker) == 0)
				j->rc = store_clear_part(j->id, ART_GATE, j->err);
		}
		break;
	case OP_REAPPLY: j->rc = store_reapply(j->id, j->err); break;
	case OP_RESTORE: j->rc = store_restore(j->id, j->err); break;
	}
}

static void job_done(void *ctx) {
	Job *j = ctx;
	app_forget_artwork(j->id);
	if (j->rc != 0) {
		dialog_message("That didn't work", j->err);
	} else {
		static const char *const done[] = {"Back to the original", "Your artwork is back", "Original look restored"};
		app_changed(j->icon, done[j->op]);
	}
	if (is_open && strcmp(j->id, app_id) == 0)
		reload();
	free(j);
	app_refresh_apps();
}

static void run(Op op, ArtPart part) {
	if (app_require_unsafe())
		return;
	Job *j = calloc(1, sizeof(*j));
	if (!j)
		return;
	j->op = op;
	j->part = part;
	j->icon = op == OP_CLEAR ? part == ART_ICON : store_has_custom(app_id, ART_ICON);
	snprintf(j->id, sizeof(j->id), "%s", app_id);
	static const char *const labels[] = {"Putting the original back...", "Putting your artwork back...",
	                                     "Restoring the original look..."};
	app_run(labels[op], job_work, job_done, j);
}

// --- menus ----------------------------------------------------------------------

static ArtPart menu_part;

static void on_picture(const char *const *paths, int count) {
	if (count > 0)
		editor_open(app_id, menu_part, paths[0]);
}

// The app's current artwork comes first in the picker, so it can be re-framed.
static void choose_picture(void) {
	static const char *const titles[ART_COUNT] = {"Choose a bubble icon", "Choose a LiveArea background",
	                                              "Choose a start gate picture"};
	char current[1 + 2 * LIVEAREA_MAX_FILES][512];
	const char *extras[1 + 2 * LIVEAREA_MAX_FILES];
	int n = 0;
	store_appmeta_path(app_id, "icon0.png", current[n], sizeof(current[0]));
	n++;
	LiveArea la;
	if (appdb_livearea(app_id, &la) == 0)
		for (int i = 0; i < la.bg_count + la.gate_count && n < 1 + 2 * LIVEAREA_MAX_FILES; i++) {
			char rel[LIVEAREA_NAME_SIZE + 32];
			snprintf(rel, sizeof(rel), "livearea/contents/%s", i < la.bg_count ? la.bg[i] : la.gate[i - la.bg_count]);
			store_appmeta_path(app_id, rel, current[n], sizeof(current[0]));
			n++;
		}
	int kept = 0;
	for (int i = 0; i < n; i++)
		if (fs_exists(current[i]))
			extras[kept++] = current[i];
	picture_picker_open_with(titles[menu_part], 1, "This app", extras, kept, on_picture);
}

static void on_part_menu(int choice) {
	if (choice == 0)
		choose_picture();
	else if (choice == 1)
		run(OP_CLEAR, menu_part);
}

static void on_confirm_restore(int choice) {
	if (choice == 0)
		run(OP_RESTORE, ART_ICON);
}

static void activate(int row) {
	static const char *const names[] = {"Bubble icon", "LiveArea background", "Start gate"};
	switch (row) {
	case ROW_ICON:
	case ROW_BG:
	case ROW_GATE: {
		menu_part = row == ROW_ICON ? ART_ICON : row == ROW_BG ? ART_BG : ART_GATE;
		if (!store_has_custom(app_id, menu_part)) {
			choose_picture();
			break;
		}
		static const char *const options[] = {"Choose a picture...", "Back to the original"};
		dialog_menu(names[menu_part], options, 2, 0, on_part_menu);
		break;
	}
	case ROW_REAPPLY: run(OP_REAPPLY, ART_ICON); break;
	case ROW_RESTORE:
		dialog_confirm("Restore the original look?",
		               "Every change Imago made to this app is undone and its original artwork put back. "
		               "The backup stays on your memory card.",
		               "Restore", 1, on_confirm_restore);
		break;
	}
}

// --- input & drawing ------------------------------------------------------------

void appdetail_update(const Input *in) {
	if (closing) {
		appear = ui_approach(appear, 0.0f, 20.0f);
		if (appear < 0.02f)
			is_open = 0;
		return;
	}
	appear = ui_approach(appear, 1.0f, 16.0f);
	if ((in->repeat & SCE_CTRL_UP) && focus > 0)
		focus--;
	if ((in->repeat & SCE_CTRL_DOWN) && focus < row_count - 1)
		focus++;
	if (in->pressed & BTN_OK)
		activate(rows[focus]);
	if (in->pressed & BTN_BACK)
		closing = 1;
	if (in->tapped) {
		if (in->tap_y < HEADER_H && in->tap_x < 200) {
			closing = 1;
		} else if (in->tap_x >= MARGIN && in->tap_x <= MARGIN + ROWS_W && in->tap_y >= ROWS_Y) {
			int i = (int)((in->tap_y - ROWS_Y) / (ROW_H + ROW_GAP));
			if (i >= 0 && i < row_count) {
				focus = i;
				activate(rows[i]);
			}
		}
	}
}

static void part_value(ArtPart part, char *out, size_t size) {
	if (store_has_custom(app_id, part))
		snprintf(out, size, state == STORE_DRIFTED ? "Custom (needs reapplying)" : "Custom");
	else
		snprintf(out, size, "Original");
}

static void draw_row(int index, float y, float a) {
	int row = rows[index], focused = index == focus;
	float x = MARGIN, w = ROWS_W;
	ui_rrect(x, y, w, ROW_H, 12.0f, ui_alpha(focused ? C_SURFACE2 : C_SURFACE, a));
	char value[64] = "";
	const char *label = "";
	uint32_t color = C_TEXT;
	switch (row) {
	case ROW_ICON: label = "Bubble icon", part_value(ART_ICON, value, sizeof(value)); break;
	case ROW_BG: label = "LiveArea background", part_value(ART_BG, value, sizeof(value)); break;
	case ROW_GATE: label = "Start gate", part_value(ART_GATE, value, sizeof(value)); break;
	case ROW_REAPPLY:
		label = "Reapply my artwork";
		color = C_WARNING;
		break;
	case ROW_RESTORE:
		label = "Restore the original look";
		color = C_DANGER;
		break;
	}
	ui_text(FONT_BOLD, x + 16.0f, y + 31.0f, ui_alpha(color, a), label);
	if (value[0])
		ui_text_right(FONT_SMALL, x + w - 16.0f, y + 30.0f, ui_alpha(C_TEXT3, a), value);
	if (focused)
		ui_rrect_outline(x - 3.0f, y - 3.0f, w + 6.0f, ROW_H + 6.0f, 15.0f, 2.0f,
		                 ui_alpha(C_ACCENT, a * (0.75f + 0.25f * sinf(ui_time() * 4.0f))));
}

static void draw_livearea(float a, float dy) {
	float x = PREVIEW_X, y = PREVIEW_Y + dy, w = PREVIEW_W, h = PREVIEW_H, s = w / BG_W;
	ui_rrect(x, y, w, h, 8.0f, ui_alpha(C_SURFACE2, a));
	char rel[LIVEAREA_NAME_SIZE + 32], path[256];
	if (la.bg_count) {
		snprintf(rel, sizeof(rel), "livearea/contents/%s", la.bg[0]);
		store_appmeta_path(app_id, rel, path, sizeof(path));
		ImageRef bg = images_file(path);
		if (bg.tex)
			ui_image(bg.tex, x, y, w, h, 8, 8, 8, 8, ui_alpha(0xFFFFFFFF, a * bg.alpha));
	} else {
		ui_text_center(FONT_SMALL, x + w * 0.5f, y + h * 0.5f, ui_alpha(C_TEXT3, a), "No LiveArea background");
	}
	if (la.gate_count) {
		snprintf(rel, sizeof(rel), "livearea/contents/%s", la.gate[0]);
		store_appmeta_path(app_id, rel, path, sizeof(path));
		ImageRef gate = images_file(path);
		int gx, gy;
		int known = livearea_gate_origin(la.style, &gx, &gy) == 0;
		// Unknown layouts: show the gate in the corner, clearly as a sample.
		float bx = known ? x + gx * s : x + w - GATE_W * s - 10.0f;
		float by = known ? y + gy * s : y + h - GATE_H * s - 10.0f;
		ui_shadow(bx, by, GATE_W * s, GATE_H * s, 6.0f, a * 0.6f);
		if (gate.tex)
			ui_image(gate.tex, bx, by, GATE_W * s, GATE_H * s, 4, 4, 4, 4, ui_alpha(0xFFFFFFFF, a * gate.alpha));
		ui_rrect_outline(bx, by, GATE_W * s, GATE_H * s, 4.0f, 1.5f, ui_alpha(0xFFFFFFFF, a * 0.7f));
		if (!known)
			ui_text_right(FONT_SMALL, x + w - 10.0f, by - 6.0f, ui_alpha(C_TEXT2, a), "Start gate");
	}
	char caption[96];
	snprintf(caption, sizeof(caption), "LiveArea now \xC2\xB7 layout %s", la.style[0] ? la.style : "?");
	ui_text(FONT_SMALL, x, y + h + 22.0f, ui_alpha(C_TEXT3, a), caption);
}

void appdetail_draw(void) {
	if (!is_open)
		return;
	float a = appear, dy = (1.0f - a) * 20.0f;
	ui_rect(0, 0, SCREEN_W, SCREEN_H, ui_alpha(C_BG, a));
	ui_vgradient(0, 0, SCREEN_W, 160.0f, ui_alpha(C_ACCENT2, 0.10f * a), ui_alpha(C_ACCENT2, 0.0f));

	ui_glyph(ui_back_glyph(), MARGIN + 10.0f, 34.0f, a);
	ui_text(FONT_BOLD, MARGIN + 28.0f, 40.0f, ui_alpha(C_TEXT2, a), "Back");

	// Bubble and title
	char path[256], text[160];
	float iy = 84.0f + dy, size = 104.0f, r = size * 0.5f;
	store_appmeta_path(app_id, "icon0.png", path, sizeof(path));
	ImageRef icon = images_file(path);
	if (icon.tex)
		ui_image(icon.tex, MARGIN, iy, size, size, r, r, r, r, ui_alpha(0xFFFFFFFF, a * icon.alpha));
	else
		ui_rrect(MARGIN, iy, size, size, r, ui_alpha(C_SURFACE3, a));
	float tx = MARGIN + size + 18.0f;
	ui_text_fit(FONT_HEADING, title, (int)(ROWS_W - size - 18.0f), text, sizeof(text));
	ui_text(FONT_HEADING, tx, iy + 42.0f, ui_alpha(C_TEXT, a), text);
	ui_text(FONT_BODY, tx, iy + 68.0f, ui_alpha(C_TEXT3, a), app_id);
	if (state != STORE_STOCK) {
		const char *badge = state == STORE_DRIFTED ? "NEEDS REAPPLYING" : "CUSTOM";
		ui_badge(tx, iy + 80.0f, badge, state == STORE_DRIFTED ? ui_alpha(C_WARNING, 0.9f) : C_ACCENT,
		         state == STORE_DRIFTED ? C_BG : C_ON_ACCENT);
	}

	for (int i = 0; i < row_count; i++)
		draw_row(i, ROWS_Y + dy + (float)i * (ROW_H + ROW_GAP), a);

	draw_livearea(a, dy);

	Hint hints[2] = {{ui_back_glyph(), "Back"}, {ui_ok_glyph(), "Select"}};
	app_footer(hints, 2, NULL);
}
