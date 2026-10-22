// Apps: every installed bubble, with its customization state.
#include "config.h"
#include "dialog.h"
#include "images.h"
#include "app.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLS 6
#define CELL_W 144.0f
#define CELL_H 150.0f
#define ICON 96.0f
#define GRID_X ((SCREEN_W - COLS * CELL_W) * 0.5f)
#define BANNER_H 52.0f
#define GRID_BOTTOM (FOOTER_Y - 4.0f)

static int selected;
static char selected_id[16];
static float scroll, scroll_target;

// Indexes into g_apps of the apps shown (system apps only when enabled).
static int visible[1024];
static int visible_count;

static void build_visible(void) {
	visible_count = 0;
	for (int i = 0; i < g_apps.count && visible_count < (int)(sizeof(visible) / sizeof(visible[0])); i++)
		if (g_config.show_system || !g_apps.items[i].system)
			visible[visible_count++] = i;
	// Keep the same app selected when the list is rebuilt.
	for (int v = 0; v < visible_count; v++)
		if (strcmp(g_apps.items[visible[v]].id, selected_id) == 0)
			selected = v;
	if (selected >= visible_count)
		selected = visible_count > 0 ? visible_count - 1 : 0;
}

static int drifted_count(void) {
	int n = 0;
	for (int i = 0; i < g_apps.count; i++)
		n += g_apps.items[i].state == STORE_DRIFTED;
	return n;
}

// --- reapply all ----------------------------------------------------------------

typedef struct {
	int count, failed;
	char ids[512][16];
	char err[STORE_ERROR_SIZE];
} ReapplyJob;

static void reapply_work(void *ctx, NetClient *net) {
	(void)net;
	ReapplyJob *job = ctx;
	for (int i = 0; i < job->count; i++) {
		char err[STORE_ERROR_SIZE] = "";
		if (store_reapply(job->ids[i], err) != 0) {
			job->failed++;
			snprintf(job->err, sizeof(job->err), "%s: %s", job->ids[i], err);
		}
	}
}

static void reapply_done(void *ctx) {
	ReapplyJob *job = ctx;
	for (int i = 0; i < job->count; i++)
		app_forget_artwork(job->ids[i]);
	if (job->failed) {
		dialog_message("Some apps couldn't be updated", job->err);
	} else {
		char text[80];
		int icons = 0;
		for (int i = 0; i < job->count; i++)
			icons |= store_has_custom(job->ids[i], ART_ICON);
		snprintf(text, sizeof(text), "Custom look put back on %d app%s", job->count, job->count == 1 ? "" : "s");
		app_changed(icons, text);
	}
	free(job);
	app_refresh_apps();
}

void apps_reapply_all(void) {
	if (app_require_unsafe())
		return;
	ReapplyJob *job = calloc(1, sizeof(*job));
	if (!job)
		return;
	for (int i = 0; i < g_apps.count && job->count < 512; i++)
		if (g_apps.items[i].state == STORE_DRIFTED)
			snprintf(job->ids[job->count++], sizeof(job->ids[0]), "%s", g_apps.items[i].id);
	if (job->count == 0) {
		free(job);
		return;
	}
	app_run("Putting your artwork back...", reapply_work, reapply_done, job);
}

static void on_confirm_reapply(int choice) {
	if (choice == 0)
		apps_reapply_all();
}

// --- input & drawing ------------------------------------------------------------

static float grid_top(void) {
	return HEADER_H + 14.0f + (drifted_count() > 0 ? BANNER_H + 10.0f : 0.0f);
}

static float max_scroll(void) {
	int rows = (visible_count + COLS - 1) / COLS;
	float m = rows * CELL_H + 12.0f - (GRID_BOTTOM - grid_top());
	return m > 0 ? m : 0;
}

// --- restore all ----------------------------------------------------------------

typedef struct {
	int count, failed, icons;
	char ids[512][16];
	char err[STORE_ERROR_SIZE];
} RestoreJob;

static void restore_work(void *ctx, NetClient *net) {
	(void)net;
	RestoreJob *job = ctx;
	for (int i = 0; i < job->count; i++) {
		char err[STORE_ERROR_SIZE] = "";
		if (store_restore(job->ids[i], err) != 0) {
			job->failed++;
			snprintf(job->err, sizeof(job->err), "%s: %s", job->ids[i], err);
		}
	}
}

static void restore_done(void *ctx) {
	RestoreJob *job = ctx;
	for (int i = 0; i < job->count; i++)
		app_forget_artwork(job->ids[i]);
	if (job->failed)
		dialog_message("Some apps couldn't be restored", job->err);
	else
		app_changed(job->icons, "Every app has its original look again");
	free(job);
	app_refresh_apps();
}

static void on_confirm_restore_all(int choice) {
	if (choice != 0 || app_require_unsafe())
		return;
	RestoreJob *job = calloc(1, sizeof(*job));
	if (!job)
		return;
	for (int i = 0; i < g_apps.count && job->count < 512; i++)
		if (g_apps.items[i].state != STORE_STOCK) {
			job->icons |= store_has_custom(g_apps.items[i].id, ART_ICON);
			snprintf(job->ids[job->count++], sizeof(job->ids[0]), "%s", g_apps.items[i].id);
		}
	app_run("Restoring the original artwork...", restore_work, restore_done, job);
}

void apps_restore_all(void) {
	int customized = 0;
	for (int i = 0; i < g_apps.count; i++)
		customized += g_apps.items[i].state != STORE_STOCK;
	if (customized == 0) {
		app_toast("No app has been customized");
		return;
	}
	dialog_confirm("Restore every app?",
	               "Every change Imago made to your apps is undone and all their original artwork put back. "
	               "Your backups and pictures stay on the memory card.",
	               "Restore all", 1, on_confirm_restore_all);
}

void apps_screen_update(const Input *in) {
	build_visible();
	int count = visible_count;
	if (count > 0) {
		if ((in->repeat & SCE_CTRL_LEFT) && selected > 0)
			selected--;
		if ((in->repeat & SCE_CTRL_RIGHT) && selected < count - 1)
			selected++;
		if ((in->repeat & SCE_CTRL_UP) && selected >= COLS)
			selected -= COLS;
		if ((in->repeat & SCE_CTRL_DOWN) && selected + COLS < count)
			selected += COLS;
		snprintf(selected_id, sizeof(selected_id), "%s", g_apps.items[visible[selected]].id);
		if (in->pressed & BTN_OK)
			appdetail_open(selected_id);
	}
	if ((in->pressed & SCE_CTRL_TRIANGLE) && drifted_count() > 0) {
		char message[240];
		int n = drifted_count();
		snprintf(message, sizeof(message),
		         "%d app%s lost %s custom look, usually after an update or a database rebuild. Imago "
		         "kept everything and can put it back now.",
		         n, n == 1 ? "" : "s", n == 1 ? "its" : "their");
		dialog_confirm("Reapply your artwork?", message, "Reapply", 0, on_confirm_reapply);
	}

	float top = grid_top();
	if (in->dragging)
		scroll = scroll_target = scroll_target - in->drag_dy;
	if (in->tapped && in->tap_y >= top && in->tap_y <= GRID_BOTTOM) {
		int col = (int)((in->tap_x - GRID_X) / CELL_W);
		int row = (int)((in->tap_y - top + scroll) / CELL_H);
		int i = row * COLS + col;
		if (col >= 0 && col < COLS && i >= 0 && i < count) {
			selected = i;
			snprintf(selected_id, sizeof(selected_id), "%s", g_apps.items[visible[i]].id);
			appdetail_open(selected_id);
		}
	}
	if (!in->dragging && count > 0) {
		float y = (float)(selected / COLS) * CELL_H;
		if (y < scroll_target)
			scroll_target = y;
		if (y + CELL_H > scroll_target + (GRID_BOTTOM - top))
			scroll_target = y + CELL_H - (GRID_BOTTOM - top);
	}
	if (scroll_target > max_scroll())
		scroll_target = max_scroll();
	if (scroll_target < 0)
		scroll_target = 0;
	scroll = ui_approach(scroll, scroll_target, 16.0f);
}

static void draw_cell(int v, float x, float y) {
	const AppItem *it = &g_apps.items[visible[v]];
	int focused = v == selected;
	float cx = x + CELL_W * 0.5f, iy = y + 10.0f;
	if (focused)
		ui_rrect(x + 6.0f, y + 2.0f, CELL_W - 12.0f, CELL_H - 8.0f, 16.0f, C_SURFACE2);

	char path[256];
	store_appmeta_path(it->id, "icon0.png", path, sizeof(path));
	ImageRef img = images_file(path);
	float r = ICON * 0.5f;
	if (img.tex)
		ui_image(img.tex, cx - r, iy, ICON, ICON, r, r, r, r, ui_alpha(0xFFFFFFFF, img.alpha));
	else
		ui_rrect(cx - r, iy, ICON, ICON, r, C_SURFACE3);

	char title[160];
	ui_text_fit(FONT_SMALL, it->title, (int)CELL_W - 18, title, sizeof(title));
	ui_text_center(FONT_SMALL, cx, iy + ICON + 22.0f, focused ? C_TEXT : C_TEXT2, title);

	const char *badge = NULL;
	uint32_t bg = 0, fg = 0;
	if (it->state == STORE_DRIFTED)
		badge = "REAPPLY", bg = ui_alpha(C_WARNING, 0.9f), fg = C_BG;
	else if (it->state == STORE_APPLIED)
		badge = "CUSTOM", bg = C_ACCENT, fg = C_ON_ACCENT;
	if (badge)
		ui_badge(cx - (float)ui_badge_width(badge) * 0.5f, iy + ICON - 14.0f, badge, bg, fg);

	if (focused)
		ui_ring(cx, iy + r, r + 5.0f, 2.5f, 0, 6.2832f, ui_alpha(C_ACCENT, 0.75f + 0.25f * sinf(ui_time() * 4.0f)));
}

void apps_screen_draw(void) {
	build_visible();
	float top = grid_top();
	int drifted = drifted_count();
	if (drifted > 0) {
		float y = HEADER_H + 10.0f;
		ui_rrect(MARGIN, y, SCREEN_W - 2 * MARGIN, BANNER_H, 12.0f, ui_alpha(C_WARNING, 0.14f));
		char line[160];
		snprintf(line, sizeof(line), "%d app%s lost %s custom look", drifted, drifted == 1 ? "" : "s",
		         drifted == 1 ? "its" : "their");
		ui_text(FONT_BOLD, MARGIN + 16.0f, y + 21.0f, C_WARNING, line);
		ui_text(FONT_SMALL, MARGIN + 16.0f, y + 40.0f, C_TEXT2,
		        "Probably an app update or a database rebuild. Your artwork is saved.");
		ui_hint(SCREEN_W - MARGIN - 16.0f, y + BANNER_H * 0.5f, GLYPH_TRIANGLE, "Reapply all");
	}
	if (!g_db_writable) {
		ui_text_center(FONT_SMALL, SCREEN_W * 0.5f, top - 2.0f, C_WARNING,
		               "Unsafe homebrew is off: you can look around, but not change anything.");
	}

	if (g_apps_loading && g_apps.count == 0) {
		ui_spinner(SCREEN_W * 0.5f, top + 140.0f, 14.0f, C_ACCENT);
	} else if (visible_count == 0) {
		ui_text_center(FONT_TITLE, SCREEN_W * 0.5f, top + 120.0f, C_TEXT, "No apps found");
		ui_text_center(FONT_BODY, SCREEN_W * 0.5f, top + 150.0f, C_TEXT2,
		               g_db_writable ? "Imago reads the bubbles from ur0:appmeta, which looks empty."
		                        : "Enable unsafe homebrew in HENkaku Settings so Imago can read your apps.");
	} else {
		vita2d_enable_clipping();
		vita2d_set_clip_rectangle(0, (int)top, SCREEN_W, (int)GRID_BOTTOM);
		for (int v = 0; v < visible_count; v++) {
			float x = GRID_X + (float)(v % COLS) * CELL_W;
			float y = top + (float)(v / COLS) * CELL_H - scroll;
			if (y + CELL_H < top || y > GRID_BOTTOM)
				continue;
			draw_cell(v, x, y);
		}
		vita2d_disable_clipping();
	}

	char status[64];
	int custom = 0;
	for (int i = 0; i < g_apps.count; i++)
		custom += g_apps.items[i].state != STORE_STOCK;
	snprintf(status, sizeof(status), "%d apps \xC2\xB7 %d customized", visible_count, custom);
	Hint hints[1] = {{ui_ok_glyph(), "Change look"}};
	app_footer(hints, visible_count > 0 ? 1 : 0, status);
}
