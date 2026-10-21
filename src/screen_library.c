// Library: installed and local themes, with uninstall and update actions.
#include "app.h"
#include "dialog.h"
#include "fs.h"
#include "images.h"
#include "jobs.h"
#include "tasks.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIST_TOP (HEADER_H + 56.0f)
#define LIST_BOTTOM (FOOTER_Y - 8.0f)
#define ROW_H 84.0f
#define ROW_GAP 10.0f
#define THUMB_W 128.0f
#define THUMB_H 72.0f
#define MAX_UPDATES 256

static int selected;
static float scroll, scroll_target;
static LibraryItem action_item; // the item an open action menu refers to

// Latest repository hashes from the last update check, by psvt id.
static struct {
	uint32_t id;
	char hash[41];
} updates[MAX_UPDATES];
static int update_count, checking;

static const char *latest_hash(uint32_t id) {
	for (int i = 0; i < update_count; i++)
		if (updates[i].id == id)
			return updates[i].hash;
	return NULL;
}

static int has_update(const LibraryItem *it) {
	if (!it->psvt_id || !it->psvt_hash[0])
		return 0;
	const char *latest = latest_hash(it->psvt_id);
	if (!latest) {
		const PsvtTheme *seen = discover_find(it->psvt_id);
		latest = seen ? seen->hash : NULL;
	}
	return latest && latest[0] && strcmp(latest, it->psvt_hash) != 0;
}

static int unregistered_count(void) {
	int n = 0;
	for (int i = 0; i < g_library.count; i++)
		if (!g_library.items[i].registered)
			n++;
	return n;
}

// --- update check ---------------------------------------------------------------

typedef struct {
	int count;
	uint32_t ids[MAX_UPDATES];
	char hashes[MAX_UPDATES][41];
	int failures;
} UpdateJob;

static void update_work(void *ctx, NetClient *net) {
	UpdateJob *job = ctx;
	for (int i = 0; i < job->count; i++) {
		char url[128], *html = NULL;
		psvt_detail_url(url, sizeof(url), job->ids[i]);
		PsvtDetail d;
		if (net_get(net, url, &html, NULL, 8 * 1024 * 1024, PSVT_DETAIL_END_MARKER, NULL) == NET_OK &&
		    psvt_parse_detail(html, &d) == 0)
			snprintf(job->hashes[i], sizeof(job->hashes[i]), "%s", d.hash);
		else
			job->failures++;
		free(html);
	}
}

static void update_done(void *ctx) {
	UpdateJob *job = ctx;
	int found = 0;
	update_count = 0;
	for (int i = 0; i < job->count && update_count < MAX_UPDATES; i++) {
		if (!job->hashes[i][0])
			continue;
		updates[update_count].id = job->ids[i];
		snprintf(updates[update_count].hash, sizeof(updates[0].hash), "%s", job->hashes[i]);
		update_count++;
	}
	for (int i = 0; i < g_library.count; i++)
		found += has_update(&g_library.items[i]);
	char text[96];
	if (job->failures == job->count && job->count > 0)
		snprintf(text, sizeof(text), "Couldn't check for updates. Are you online?");
	else if (found)
		snprintf(text, sizeof(text), "%d update%s available", found, found == 1 ? "" : "s");
	else
		snprintf(text, sizeof(text), "All themes are up to date");
	app_toast(text);
	checking = 0;
	free(job);
}

static void check_updates(void) {
	if (checking)
		return;
	UpdateJob *job = calloc(1, sizeof(*job));
	if (!job)
		return;
	for (int i = 0; i < g_library.count && job->count < MAX_UPDATES; i++)
		if (g_library.items[i].psvt_id)
			job->ids[job->count++] = g_library.items[i].psvt_id;
	if (job->count == 0) {
		free(job);
		app_toast("No themes installed with Imago to check");
		return;
	}
	checking = 1;
	jobs_submit(update_work, update_done, job);
}

// --- actions --------------------------------------------------------------------

enum { ACT_UNINSTALL, ACT_REMOVE_ENTRY, ACT_UPDATE, ACT_REGISTER, ACT_DELETE_FILES, ACT_CUSTOMIZE };
static int action_ids[7];
static const char *action_labels[7];
static int action_count;

static void add_action(int id, const char *label) {
	action_ids[action_count] = id;
	action_labels[action_count] = label;
	action_count++;
}

static void show_unsafe_message(void) {
	dialog_message("Unsafe homebrew is disabled",
	               "Changing installed themes needs write access to the system theme list. Enable "
	               "\"Unsafe homebrew\" in HENkaku Settings, then restart Imago.");
}

static void on_confirm_uninstall(int choice) {
	if (choice != 0)
		return;
	if (action_item.registered)
		tasks_uninstall(action_item.dir, action_item.title, 0);
	else
		tasks_delete_files(action_item.dir, action_item.title);
}

static void on_action(int choice) {
	if (choice < 0)
		return;
	char message[400];
	switch (action_ids[choice]) {
	case ACT_UNINSTALL:
		snprintf(message, sizeof(message),
		         "\"%s\" will be removed and its files deleted from %s. If it's your current theme, "
		         "switch to another one in Settings first.",
		         action_item.title, action_item.dir);
		dialog_confirm("Uninstall theme?", message, "Uninstall", 1, on_confirm_uninstall);
		break;
	case ACT_DELETE_FILES:
		snprintf(message, sizeof(message), "The folder %s will be deleted from your memory card.", action_item.dir);
		dialog_confirm("Delete theme files?", message, "Delete", 1, on_confirm_uninstall);
		break;
	case ACT_REMOVE_ENTRY:
		tasks_uninstall(action_item.dir, action_item.title, 1);
		break;
	case ACT_UPDATE: {
		const char *hash = latest_hash(action_item.psvt_id);
		const PsvtTheme *seen = discover_find(action_item.psvt_id);
		if (!hash && seen)
			hash = seen->hash;
		if (hash)
			tasks_install(action_item.psvt_id, hash, action_item.title);
		break;
	}
	case ACT_REGISTER:
		tasks_register(action_item.dir, action_item.title);
		break;
	case ACT_CUSTOMIZE:
		studio_open(action_item.dir);
		break;
	}
}

static void open_actions(const LibraryItem *it) {
	if (!g_db_writable) {
		show_unsafe_message();
		return;
	}
	action_item = *it;
	action_count = 0;
	if (!it->registered) {
		add_action(ACT_REGISTER, "Add to system themes");
		if (it->deletable)
			add_action(ACT_DELETE_FILES, "!Delete files");
	} else {
		if (it->has_files)
			add_action(ACT_CUSTOMIZE, "Customize or mix...");
		if (has_update(it))
			add_action(ACT_UPDATE, "Update to latest version");
		if (it->deletable && it->has_files)
			add_action(ACT_UNINSTALL, "!Uninstall");
		add_action(ACT_REMOVE_ENTRY, it->has_files ? "Remove from system only (keep files)" : "!Remove broken entry");
	}
	dialog_menu(it->title, action_labels, action_count, 0, on_action);
}

static void register_all(void) {
	int n = 0;
	for (int i = 0; i < g_library.count; i++) {
		if (!g_library.items[i].registered) {
			tasks_register(g_library.items[i].dir, g_library.items[i].title);
			n++;
		}
	}
	char text[64];
	snprintf(text, sizeof(text), "Adding %d local theme%s", n, n == 1 ? "" : "s");
	app_toast(text);
}

static void on_confirm_register_all(int choice) {
	if (choice == 0)
		register_all();
}

// --- input & drawing ------------------------------------------------------------

static float max_scroll(void) {
	float h = (float)g_library.count * (ROW_H + ROW_GAP) - ROW_GAP + 24.0f;
	float m = h - (LIST_BOTTOM - LIST_TOP);
	return m > 0 ? m : 0;
}

void library_screen_update(const Input *in) {
	int count = g_library.count;
	if (selected >= count)
		selected = count > 0 ? count - 1 : 0;
	if ((in->repeat & SCE_CTRL_UP) && selected > 0)
		selected--;
	if ((in->repeat & SCE_CTRL_DOWN) && selected < count - 1)
		selected++;
	if (in->pressed & SCE_CTRL_TRIANGLE)
		check_updates();
	if ((in->pressed & SCE_CTRL_SQUARE) && unregistered_count() > 0) {
		if (!g_db_writable) {
			show_unsafe_message();
		} else {
			char msg[160];
			int n = unregistered_count();
			snprintf(msg, sizeof(msg), "%d theme folder%s on your memory card %s not in the system theme list yet.",
			         n, n == 1 ? "" : "s", n == 1 ? "is" : "are");
			dialog_confirm("Add local themes?", msg, "Add all", 0, on_confirm_register_all);
		}
	}
	if ((in->pressed & BTN_OK) && count > 0)
		open_actions(&g_library.items[selected]);

	if (in->dragging)
		scroll = scroll_target = scroll_target - in->drag_dy;
	if (in->tapped && in->tap_y >= LIST_TOP && in->tap_y <= LIST_BOTTOM) {
		int i = (int)(((float)in->tap_y - LIST_TOP + scroll) / (ROW_H + ROW_GAP));
		if (i >= 0 && i < count) {
			selected = i;
			open_actions(&g_library.items[i]);
		}
	}
	if (!in->dragging) {
		float top = (float)selected * (ROW_H + ROW_GAP);
		if (top < scroll_target)
			scroll_target = top;
		if (top + ROW_H + 12.0f > scroll_target + (LIST_BOTTOM - LIST_TOP))
			scroll_target = top + ROW_H + 12.0f - (LIST_BOTTOM - LIST_TOP);
	}
	if (scroll_target > max_scroll())
		scroll_target = max_scroll();
	if (scroll_target < 0)
		scroll_target = 0;
	scroll = ui_approach(scroll, scroll_target, 16.0f);
}

static void draw_row(int index, float y) {
	const LibraryItem *it = &g_library.items[index];
	int focused = index == selected;
	float x = MARGIN, w = SCREEN_W - 2 * MARGIN;
	ui_rrect(x, y, w, ROW_H, 14.0f, focused ? C_SURFACE2 : C_SURFACE);

	float tx = x + 8.0f, ty = y + (ROW_H - THUMB_H) * 0.5f;
	ui_rrect(tx, ty, THUMB_W, THUMB_H, 10.0f, C_SURFACE3);
	if (it->thumb[0]) {
		ImageRef img = images_file(it->thumb);
		if (img.tex)
			ui_image(img.tex, tx, ty, THUMB_W, THUMB_H, 10, 10, 10, 10, RGBA8(255, 255, 255, (int)(255 * img.alpha)));
	}

	float cx = tx + THUMB_W + 18.0f, cw = w - THUMB_W - 220.0f;
	char text[192];
	ui_text_fit(FONT_TITLE, it->title, (int)cw, text, sizeof(text));
	ui_text(FONT_TITLE, cx, y + 34.0f, C_TEXT, text);

	char size[24] = "", meta[192];
	if (it->size > 0)
		fs_format_size(size, sizeof(size), it->size);
	const char *slash = strchr(it->dir, ':');
	char device[8] = "";
	if (slash && slash - it->dir < 6)
		snprintf(device, sizeof(device), "%.*s", (int)(slash - it->dir + 1), it->dir);
	snprintf(meta, sizeof(meta), "%s%s%s%s%s", it->provider[0] ? it->provider : "Unknown author",
	         size[0] ? "  \xC2\xB7  " : "", size, device[0] ? "  \xC2\xB7  " : "", device);
	ui_text_fit(FONT_BODY, meta, (int)cw, text, sizeof(text));
	ui_text(FONT_BODY, cx, y + 59.0f, C_TEXT3, text);

	// Status chips, right-aligned
	float bx = x + w - 18.0f;
	Task task;
	int running = it->psvt_id && tasks_find(it->psvt_id, &task) && task.state <= TASK_FINISHING;
	const char *badge = NULL;
	uint32_t bg = 0, fg = 0;
	if (running) {
		badge = "UPDATING", bg = ui_alpha(C_ACCENT, 0.2f), fg = C_ACCENT;
	} else if (!it->registered) {
		badge = "NOT ADDED", bg = ui_alpha(C_WARNING, 0.18f), fg = C_WARNING;
	} else if (!it->has_files) {
		badge = "MISSING FILES", bg = ui_alpha(C_DANGER, 0.18f), fg = C_DANGER;
	} else if (has_update(it)) {
		badge = "UPDATE", bg = ui_alpha(C_WARNING, 0.18f), fg = C_WARNING;
	}
	if (badge)
		ui_badge(bx - (float)ui_badge_width(badge), y + (ROW_H - 20.0f) * 0.5f, badge, bg, fg);

	if (focused)
		ui_rrect_outline(x - 4.0f, y - 4.0f, w + 8.0f, ROW_H + 8.0f, 18.0f, 2.5f,
		                 ui_alpha(C_ACCENT, 0.75f + 0.25f * sinf(ui_time() * 4.0f)));
}

void library_screen_draw(void) {
	char summary[128];
	int local = unregistered_count();
	int installed = g_library.count - local;
	snprintf(summary, sizeof(summary), "%d installed theme%s", installed, installed == 1 ? "" : "s");
	ui_text(FONT_TITLE, MARGIN, HEADER_H + 30.0f, C_TEXT, summary);
	float sx = MARGIN + (float)ui_text_width(FONT_TITLE, summary) + 14.0f;
	if (local > 0) {
		char label[48];
		snprintf(label, sizeof(label), "%d NOT ADDED", local);
		ui_badge(sx, HEADER_H + 14.0f, label, ui_alpha(C_WARNING, 0.18f), C_WARNING);
	}
	if (checking) {
		ui_spinner(SCREEN_W - MARGIN - 8.0f, HEADER_H + 24.0f, 8.0f, C_ACCENT);
		ui_text_right(FONT_SMALL, SCREEN_W - MARGIN - 24.0f, HEADER_H + 29.0f, C_TEXT3, "Checking for updates");
	}

	float top = LIST_TOP;
	if (!g_db_writable) {
		float h = 52.0f;
		ui_rrect(MARGIN, top - 8.0f, SCREEN_W - 2 * MARGIN, h, 12.0f, ui_alpha(C_WARNING, 0.14f));
		ui_text(FONT_BOLD, MARGIN + 16.0f, top + 13.0f, C_WARNING, "Unsafe homebrew is disabled");
		ui_text(FONT_SMALL, MARGIN + 16.0f, top + 33.0f, C_TEXT2,
		        "Enable it in HENkaku Settings and restart Imago to install or remove themes.");
		top += h + 4.0f;
	}

	if (g_library_loading && g_library.count == 0) {
		ui_spinner(SCREEN_W * 0.5f, top + 120.0f, 14.0f, C_ACCENT);
	} else if (g_library.count == 0) {
		ui_text_center(FONT_TITLE, SCREEN_W * 0.5f, top + 110.0f, C_TEXT, "No custom themes yet");
		ui_text_center(FONT_BODY, SCREEN_W * 0.5f, top + 140.0f, C_TEXT2,
		               "Themes you install from Discover will show up here.");
	} else {
		vita2d_enable_clipping();
		vita2d_set_clip_rectangle(0, (int)top - 8, SCREEN_W, (int)LIST_BOTTOM + 6);
		for (int i = 0; i < g_library.count; i++) {
			float y = top + (float)i * (ROW_H + ROW_GAP) - scroll;
			if (y + ROW_H < top - 10.0f || y > LIST_BOTTOM)
				continue;
			draw_row(i, y);
		}
		vita2d_disable_clipping();
	}

	Hint hints[3];
	int n = 0;
	if (local > 0)
		hints[n++] = (Hint){GLYPH_SQUARE, "Add local themes"};
	hints[n++] = (Hint){GLYPH_TRIANGLE, "Check for updates"};
	if (g_library.count > 0)
		hints[n++] = (Hint){ui_ok_glyph(), "Options"};
	app_footer(hints, n, NULL);
}
