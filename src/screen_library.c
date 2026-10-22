// Library: installed and local themes in a grid, organised into your own
// folders, with uninstall, update, customise and move actions.
#include "app.h"
#include "dialog.h"
#include "folders.h"
#include "fs.h"
#include "images.h"
#include "ime.h"
#include "jobs.h"
#include "tasks.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

enum { ACT_UNINSTALL, ACT_REMOVE_ENTRY, ACT_UPDATE, ACT_REGISTER, ACT_DELETE_FILES, ACT_CUSTOMIZE, ACT_MOVE };
static int action_ids[8];
static const char *action_labels[8];
static int action_count;

static void add_action(int id, const char *label) {
	action_ids[action_count] = id;
	action_labels[action_count] = label;
	action_count++;
}

static void open_move_menu(void);

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
	case ACT_MOVE:
		open_move_menu();
		break;
	}
}

// Organising into folders works without write access to the system; the
// other actions need it (the banner above the grid says how to get it).
static void open_actions(const LibraryItem *it) {
	action_item = *it;
	action_count = 0;
	if (g_db_writable && !it->registered) {
		add_action(ACT_REGISTER, "Add to system themes");
		if (it->deletable)
			add_action(ACT_DELETE_FILES, "!Delete files");
	} else if (g_db_writable) {
		if (it->has_files)
			add_action(ACT_CUSTOMIZE, "Customize or mix...");
		if (has_update(it))
			add_action(ACT_UPDATE, "Update to latest version");
		if (it->deletable && it->has_files)
			add_action(ACT_UNINSTALL, "!Uninstall");
		add_action(ACT_REMOVE_ENTRY, it->has_files ? "Remove from system only (keep files)" : "!Remove broken entry");
	}
	add_action(ACT_MOVE, "Move to folder...");
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

// --- folders ----------------------------------------------------------------------

// What the keyboard is open for.
enum { NAME_NONE, NAME_NEW_FOLDER, NAME_RENAME_FOLDER };
static int naming, naming_folder;
static int open_folder = -1;    // the folder being shown, or -1 for the top level
static int return_selected;     // the folder card to land on when leaving it
static int menu_folder;         // the folder an open folder menu refers to
static int move_targets[FOLDERS_MAX + 2];

static void move_to(int folder) {
	if (folders_move(action_item.dir, folder) != 0) {
		app_toast("Couldn't move it: too many themes in folders");
		return;
	}
	char text[160];
	if (folder >= 0)
		snprintf(text, sizeof(text), "Moved to \"%s\"", folders_name(folder));
	else
		snprintf(text, sizeof(text), "Moved out of the folder");
	app_toast(text);
}

static void on_move(int choice) {
	if (choice < 0)
		return;
	int target = move_targets[choice];
	if (target == -2) {
		naming = NAME_NEW_FOLDER;
		ime_open("Name of the new folder", "", FOLDER_NAME_MAX - 1);
	} else {
		move_to(target);
	}
}

static void open_move_menu(void) {
	static const char *labels[FOLDERS_MAX + 2];
	int n = 0, current = folders_of(action_item.dir);
	labels[n] = "New folder...";
	move_targets[n++] = -2;
	for (int f = 0; f < folders_count(); f++) {
		if (f == current)
			continue;
		labels[n] = folders_name(f);
		move_targets[n++] = f;
	}
	if (current >= 0) {
		labels[n] = "Take it out of the folder";
		move_targets[n++] = -1;
	}
	dialog_menu("Move to folder", labels, n, 0, on_move);
}

static void on_confirm_delete_folder(int choice) {
	if (choice != 0)
		return;
	folders_delete(menu_folder);
	app_toast("Folder deleted. Its themes are back in your Library.");
}

static void on_folder_menu(int choice) {
	char message[200];
	if (choice == 0) {
		naming = NAME_RENAME_FOLDER;
		naming_folder = menu_folder;
		ime_open("Rename folder", folders_name(menu_folder), FOLDER_NAME_MAX - 1);
	} else if (choice == 1) {
		snprintf(message, sizeof(message),
		         "The themes in \"%s\" go back to the top of your Library. Nothing is uninstalled.",
		         folders_name(menu_folder));
		dialog_confirm("Delete folder?", message, "Delete", 1, on_confirm_delete_folder);
	}
}

static void open_folder_menu(int folder) {
	static const char *const options[] = {"Rename...", "!Delete folder"};
	menu_folder = folder;
	dialog_menu(folders_name(folder), options, 2, 0, on_folder_menu);
}

static void finish_naming(const char *text) {
	if (naming == NAME_NEW_FOLDER) {
		int f = folders_create(text);
		if (f < 0)
			app_toast(folders_count() == FOLDERS_MAX ? "You can have up to 32 folders"
			                                         : "There's already a folder with that name");
		else
			move_to(f);
	} else if (naming == NAME_RENAME_FOLDER) {
		if (folders_rename(naming_folder, text) != 0)
			app_toast("There's already a folder with that name");
	}
	naming = NAME_NONE;
}

// --- the grid -----------------------------------------------------------------------

#define COLS 4
#define GAP 16.0f
#define CARD_W ((SCREEN_W - 2 * MARGIN - (COLS - 1) * GAP) / COLS)
#define IMAGE_H ((float)(int)(CARD_W * 128.0f / 226.0f))
#define CARD_H (IMAGE_H + 54.0f)
#define PITCH (CARD_H + 16.0f)
#define GRID_BOTTOM (FOOTER_Y - 8.0f)

// A cell: a folder (top level only) or a theme.
typedef struct {
	int folder; // >= 0 for a folder card
	int item;   // index into g_library.items for a theme card
} Cell;

static Cell *cells;
static int cell_count, cell_capacity;
static int *item_folder; // folder of each library item, worked out once per frame
static int item_capacity;
static int folder_sizes[FOLDERS_MAX];

static int theme_folder(int item) {
	return item_folder[item];
}

static void build_cells(void) {
	int need = g_library.count + folders_count();
	cell_count = 0;
	if (need > cell_capacity) {
		Cell *grown = realloc(cells, (size_t)need * sizeof(Cell));
		if (!grown)
			return;
		cells = grown;
		cell_capacity = need;
	}
	if (g_library.count > item_capacity) {
		int *grown = realloc(item_folder, (size_t)g_library.count * sizeof(int));
		if (!grown)
			return;
		item_folder = grown;
		item_capacity = g_library.count;
	}
	memset(folder_sizes, 0, sizeof(folder_sizes));
	for (int i = 0; i < g_library.count; i++) {
		int f = folders_of(g_library.items[i].dir);
		item_folder[i] = f < folders_count() ? f : -1;
		if (item_folder[i] >= 0)
			folder_sizes[item_folder[i]]++;
	}
	if (open_folder >= folders_count())
		open_folder = -1; // deleted meanwhile
	if (open_folder < 0)
		for (int f = 0; f < folders_count(); f++)
			cells[cell_count++] = (Cell){f, -1};
	for (int i = 0; i < g_library.count; i++)
		if (item_folder[i] == open_folder)
			cells[cell_count++] = (Cell){-1, i};
}

static int folder_size(int folder) {
	return folder >= 0 && folder < FOLDERS_MAX ? folder_sizes[folder] : 0;
}

static float grid_top(void) {
	return HEADER_H + 56.0f + (g_db_writable ? 0.0f : 56.0f);
}

static float view_h(void) {
	return GRID_BOTTOM - grid_top();
}

static void enter_folder(int folder) {
	return_selected = selected;
	open_folder = folder;
	selected = 0;
	scroll = scroll_target = 0.0f;
}

static void leave_folder(void) {
	open_folder = -1;
	selected = return_selected;
	scroll = scroll_target = 0.0f;
}

static void activate(int cell) {
	if (cell < 0 || cell >= cell_count)
		return;
	if (cells[cell].folder >= 0)
		enter_folder(cells[cell].folder);
	else
		open_actions(&g_library.items[cells[cell].item]);
}

void library_screen_update(const Input *in) {
	if (ime_active()) {
		char text[FOLDER_NAME_MAX];
		int r = ime_poll(text, sizeof(text));
		if (r == 1)
			finish_naming(text);
		else if (r == -1)
			naming = NAME_NONE;
		return;
	}
	build_cells();
	int count = cell_count;
	if (selected >= count)
		selected = count > 0 ? count - 1 : 0;
	if ((in->repeat & SCE_CTRL_LEFT) && selected % COLS > 0)
		selected--;
	if ((in->repeat & SCE_CTRL_RIGHT) && selected % COLS < COLS - 1 && selected < count - 1)
		selected++;
	if ((in->repeat & SCE_CTRL_UP) && selected >= COLS)
		selected -= COLS;
	if ((in->repeat & SCE_CTRL_DOWN) && selected + COLS < count)
		selected += COLS;
	else if ((in->repeat & SCE_CTRL_DOWN) && selected / COLS < (count - 1) / COLS)
		selected = count - 1; // onto a shorter last row
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
		activate(selected);
	else if ((in->pressed & SCE_CTRL_SELECT) && count > 0 && cells[selected].folder >= 0)
		open_folder_menu(cells[selected].folder);
	else if ((in->pressed & BTN_BACK) && open_folder >= 0)
		leave_folder();

	float top = grid_top();
	if (in->dragging)
		scroll = scroll_target = scroll_target - in->drag_dy;
	if (in->tapped && in->tap_y < top && open_folder >= 0) {
		leave_folder(); // the "Library ›" breadcrumb
	} else if (in->tapped && in->tap_y >= top && in->tap_y <= GRID_BOTTOM) {
		float cy = (float)in->tap_y - top + scroll;
		int row = (int)(cy / PITCH), col = (int)(((float)in->tap_x - MARGIN) / (CARD_W + GAP));
		int i = row * COLS + col;
		if (col >= 0 && col < COLS && i < count && cy - (float)row * PITCH <= CARD_H) {
			selected = i;
			activate(i);
		}
	}
	if (!in->dragging) {
		float y = (float)(selected / COLS) * PITCH;
		if (y - 8.0f < scroll_target)
			scroll_target = y - 8.0f;
		if (y + CARD_H + 12.0f > scroll_target + view_h())
			scroll_target = y + CARD_H + 12.0f - view_h();
	}
	int rows = (count + COLS - 1) / COLS;
	float max = (float)rows * PITCH - view_h();
	if (scroll_target > max)
		scroll_target = max > 0 ? max : 0;
	if (scroll_target < 0)
		scroll_target = 0;
	scroll = ui_approach(scroll, scroll_target, 16.0f);
}

// --- drawing ------------------------------------------------------------------------

static void draw_thumb(const char *path, float x, float y, float w, float h, float r_top, float r_bottom) {
	if (!path[0])
		return;
	ImageRef img = images_file(path);
	if (img.tex)
		ui_image(img.tex, x, y, w, h, r_top, r_top, r_bottom, r_bottom, RGBA8(255, 255, 255, (int)(255 * img.alpha)));
}

static void card_frame(float x, float y, int focused) {
	if (focused)
		ui_shadow(x, y, CARD_W, CARD_H, 14.0f, 1.0f);
	ui_rrect(x, y, CARD_W, CARD_H, 14.0f, focused ? C_SURFACE2 : C_SURFACE);
	ui_rrect(x, y, CARD_W, IMAGE_H, 14.0f, C_SURFACE3);
	ui_rect(x, y + IMAGE_H - 14.0f, CARD_W, 14.0f, C_SURFACE3);
}

static void card_text(float x, float y, const char *title, const char *meta) {
	char text[192];
	ui_text_fit(FONT_BOLD, title, (int)CARD_W - 24, text, sizeof(text));
	ui_text(FONT_BOLD, x + 12.0f, y + IMAGE_H + 24.0f, C_TEXT, text);
	ui_text_fit(FONT_SMALL, meta, (int)CARD_W - 24, text, sizeof(text));
	ui_text(FONT_SMALL, x + 12.0f, y + IMAGE_H + 43.0f, C_TEXT3, text);
}

// A folder shows the first four of its themes as a 2x2 mosaic.
static void draw_folder(int folder, float x, float y, int focused) {
	card_frame(x, y, focused);
	int total = folder_size(folder), shown = 0;
	int cols = total >= 2 ? 2 : 1, rows = total >= 3 ? 2 : 1;
	float w = (CARD_W - (float)(cols + 1) * 4.0f) / (float)cols;
	float h = (IMAGE_H - (float)(rows + 1) * 4.0f) / (float)rows;
	for (int i = 0; i < g_library.count && shown < cols * rows; i++) {
		if (theme_folder(i) != folder)
			continue;
		float tx = x + 4.0f + (float)(shown % cols) * (w + 4.0f);
		float ty = y + 4.0f + (float)(shown / cols) * (h + 4.0f);
		ui_rrect(tx, ty, w, h, 8.0f, C_SURFACE2);
		draw_thumb(g_library.items[i].thumb, tx, ty, w, h, 8.0f, 8.0f);
		shown++;
	}
	if (shown == 0)
		ui_text_center(FONT_SMALL, x + CARD_W * 0.5f, y + IMAGE_H * 0.5f + 5.0f, C_TEXT3, "Empty");
	// A folder tab on the label, so folders read differently from themes.
	float fx = x + 12.0f, fy = y + IMAGE_H + 12.0f;
	ui_rrect(fx, fy + 3.0f, 18.0f, 13.0f, 3.0f, C_ACCENT);
	ui_rrect(fx, fy, 8.0f, 6.0f, 2.0f, C_ACCENT);
	char text[192], meta[48];
	int n = folder_size(folder);
	snprintf(meta, sizeof(meta), "%d theme%s", n, n == 1 ? "" : "s");
	ui_text_fit(FONT_BOLD, folders_name(folder), (int)CARD_W - 50, text, sizeof(text));
	ui_text(FONT_BOLD, x + 38.0f, y + IMAGE_H + 24.0f, C_TEXT, text);
	ui_text(FONT_SMALL, x + 12.0f, y + IMAGE_H + 43.0f, C_TEXT3, meta);
	if (focused)
		ui_rrect_outline(x - 4.0f, y - 4.0f, CARD_W + 8.0f, CARD_H + 8.0f, 18.0f, 2.5f,
		                 ui_alpha(C_ACCENT, 0.75f + 0.25f * sinf(ui_time() * 4.0f)));
}

static void draw_theme(int index, float x, float y, int focused) {
	const LibraryItem *it = &g_library.items[index];
	card_frame(x, y, focused);
	draw_thumb(it->thumb, x, y, CARD_W, IMAGE_H, 14.0f, 0.0f);

	char size[24] = "", meta[192];
	if (it->size > 0)
		fs_format_size(size, sizeof(size), it->size);
	const char *slash = strchr(it->dir, ':');
	char device[8] = "";
	if (slash && slash - it->dir < 6)
		snprintf(device, sizeof(device), "%.*s", (int)(slash - it->dir + 1), it->dir);
	snprintf(meta, sizeof(meta), "%s%s%s%s%s", it->provider[0] ? it->provider : "Unknown author",
	         size[0] ? " \xC2\xB7 " : "", size, device[0] ? " \xC2\xB7 " : "", device);
	card_text(x, y, it->title, meta);

	Task task;
	int running = it->psvt_id && tasks_find(it->psvt_id, &task) && task.state <= TASK_FINISHING;
	const char *badge = NULL;
	uint32_t bg = 0, fg = 0;
	if (running) {
		badge = "UPDATING", bg = C_ACCENT, fg = C_ON_ACCENT;
	} else if (!it->registered) {
		badge = "NOT ADDED", bg = C_WARNING, fg = RGBA8(30, 24, 10, 255);
	} else if (!it->has_files) {
		badge = "MISSING FILES", bg = C_DANGER, fg = RGBA8(30, 10, 10, 255);
	} else if (has_update(it)) {
		badge = "UPDATE", bg = C_WARNING, fg = RGBA8(30, 24, 10, 255);
	}
	if (badge)
		ui_badge(x + CARD_W - 10.0f - (float)ui_badge_width(badge), y + IMAGE_H - 30.0f, badge, bg, fg);

	if (focused)
		ui_rrect_outline(x - 4.0f, y - 4.0f, CARD_W + 8.0f, CARD_H + 8.0f, 18.0f, 2.5f,
		                 ui_alpha(C_ACCENT, 0.75f + 0.25f * sinf(ui_time() * 4.0f)));
}

void library_screen_draw(void) {
	build_cells();
	char summary[160];
	int local = unregistered_count();
	if (open_folder >= 0) {
		ui_glyph(ui_back_glyph(), MARGIN + 10.0f, HEADER_H + 24.0f, 1.0f);
		ui_text(FONT_TITLE, MARGIN + 28.0f, HEADER_H + 30.0f, C_TEXT3, "Library  \xE2\x80\xBA");
		float tx = MARGIN + 28.0f + (float)ui_text_width(FONT_TITLE, "Library  \xE2\x80\xBA") + 8.0f;
		int n = folder_size(open_folder);
		snprintf(summary, sizeof(summary), "%s  \xC2\xB7  %d theme%s", folders_name(open_folder), n, n == 1 ? "" : "s");
		ui_text(FONT_TITLE, tx, HEADER_H + 30.0f, C_TEXT, summary);
	} else {
		int installed = g_library.count - local;
		snprintf(summary, sizeof(summary), "%d installed theme%s", installed, installed == 1 ? "" : "s");
		ui_text(FONT_TITLE, MARGIN, HEADER_H + 30.0f, C_TEXT, summary);
		float sx = MARGIN + (float)ui_text_width(FONT_TITLE, summary) + 14.0f;
		if (local > 0) {
			char label[48];
			snprintf(label, sizeof(label), "%d NOT ADDED", local);
			ui_badge(sx, HEADER_H + 14.0f, label, ui_alpha(C_WARNING, 0.18f), C_WARNING);
		}
	}
	if (checking) {
		ui_spinner(SCREEN_W - MARGIN - 8.0f, HEADER_H + 24.0f, 8.0f, C_ACCENT);
		ui_text_right(FONT_SMALL, SCREEN_W - MARGIN - 24.0f, HEADER_H + 29.0f, C_TEXT3, "Checking for updates");
	}

	float top = grid_top();
	if (!g_db_writable) {
		float h = 52.0f, by = HEADER_H + 48.0f;
		ui_rrect(MARGIN, by, SCREEN_W - 2 * MARGIN, h, 12.0f, ui_alpha(C_WARNING, 0.14f));
		ui_text(FONT_BOLD, MARGIN + 16.0f, by + 21.0f, C_WARNING, "Unsafe homebrew is disabled");
		ui_text(FONT_SMALL, MARGIN + 16.0f, by + 41.0f, C_TEXT2,
		        "Enable it in HENkaku Settings and restart Imago to install or remove themes.");
	}

	if (g_library_loading && g_library.count == 0) {
		ui_spinner(SCREEN_W * 0.5f, top + 120.0f, 14.0f, C_ACCENT);
	} else if (g_library.count == 0) {
		ui_text_center(FONT_TITLE, SCREEN_W * 0.5f, top + 110.0f, C_TEXT, "No custom themes yet");
		ui_text_center(FONT_BODY, SCREEN_W * 0.5f, top + 140.0f, C_TEXT2,
		               "Themes you install from Discover will show up here.");
	} else if (cell_count == 0) {
		ui_text_center(FONT_TITLE, SCREEN_W * 0.5f, top + 110.0f, C_TEXT, "This folder is empty");
		ui_text_center(FONT_BODY, SCREEN_W * 0.5f, top + 140.0f, C_TEXT2,
		               "Open a theme's options and choose Move to folder.");
	} else {
		vita2d_enable_clipping();
		vita2d_set_clip_rectangle(0, (int)top - 10, SCREEN_W, (int)GRID_BOTTOM + 4);
		int first = (int)floorf((scroll - 20.0f) / PITCH), last = (int)ceilf((scroll + view_h()) / PITCH);
		for (int row = first < 0 ? 0 : first; row <= last; row++)
			for (int col = 0; col < COLS; col++) {
				int i = row * COLS + col;
				if (i >= cell_count)
					continue;
				float x = MARGIN + (float)col * (CARD_W + GAP), y = top + (float)row * PITCH - scroll;
				if (cells[i].folder >= 0)
					draw_folder(cells[i].folder, x, y, i == selected);
				else
					draw_theme(cells[i].item, x, y, i == selected);
			}
		vita2d_disable_clipping();
	}

	Hint hints[5];
	int n = 0;
	int on_folder = cell_count > 0 && selected < cell_count && cells[selected].folder >= 0;
	if (open_folder >= 0)
		hints[n++] = (Hint){ui_back_glyph(), "Back"};
	if (local > 0)
		hints[n++] = (Hint){GLYPH_SQUARE, "Add local themes"};
	hints[n++] = (Hint){GLYPH_TRIANGLE, "Check for updates"};
	if (on_folder)
		hints[n++] = (Hint){GLYPH_SELECT, "Folder options"};
	if (cell_count > 0)
		hints[n++] = (Hint){ui_ok_glyph(), on_folder ? "Open" : "Options"};
	app_footer(hints, n, NULL);
}
