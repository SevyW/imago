// Picture picker: the memory card's pictures in a grid, one folder per chip.
#include "picture_picker.h"

#include "app.h"
#include "images.h"
#include "jobs.h"
#include "log.h"
#include "pictures.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLS 4
#define GAP 16.0f
#define CARD_W ((SCREEN_W - 2 * MARGIN - (COLS - 1) * GAP) / COLS)
#define IMAGE_H ((float)(int)(CARD_W * 544.0f / 960.0f))
#define CARD_H (IMAGE_H + 30.0f)
#define PITCH (CARD_H + 14.0f)
#define CHIP_Y (HEADER_H + 6.0f)
#define CHIP_H 32.0f
#define GRID_TOP (HEADER_H + 54.0f)
#define GRID_BOTTOM (FOOTER_Y - 8.0f)
#define VIEW_H (GRID_BOTTOM - GRID_TOP)
#define MAX_FOLDERS 32
#define MAX_PICKS 10

#define MAX_EXTRAS 24

static int is_open, scanning, scan_generation;
static Picture extras[MAX_EXTRAS];
static int extra_count;
static int max_pick;
static PicturePickFn on_done;
static char title[96];
static PictureList list;

static int folder_count, folder;
static char folder_names[MAX_FOLDERS][64];
static int folder_start[MAX_FOLDERS], folder_len[MAX_FOLDERS];
static float chip_x[MAX_FOLDERS], chip_w[MAX_FOLDERS], chip_scroll;

static int selected;
static float scroll, scroll_target;
static int picks[MAX_PICKS], pick_count; // indices into list, in pick order
static int viewer = -1;

// --- scanning -----------------------------------------------------------------------

typedef struct {
	int generation;
	PictureList result;
} ScanJob;

static void scan_work(void *ctx, NetClient *net) {
	(void)net;
	pictures_scan(&((ScanJob *)ctx)->result);
}

// The list is sorted by folder, so each folder is one contiguous run.
static void group_folders(void) {
	folder_count = 0;
	for (int i = 0; i < list.count; i++) {
		if (folder_count > 0 && strcmp(folder_names[folder_count - 1], list.items[i].folder) == 0) {
			folder_len[folder_count - 1]++;
			continue;
		}
		if (folder_count == MAX_FOLDERS)
			break;
		snprintf(folder_names[folder_count], sizeof(folder_names[0]), "%s", list.items[i].folder);
		folder_start[folder_count] = i;
		folder_len[folder_count] = 1;
		folder_count++;
	}
}

static void scan_done(void *ctx) {
	ScanJob *job = ctx;
	if (job->generation == scan_generation) {
		pictures_free(&list);
		list = job->result;
		if (extra_count > 0) { // the extras go first, as a folder of their own
			Picture *merged = malloc((size_t)(list.count + extra_count) * sizeof(Picture));
			if (merged) {
				memcpy(merged, extras, (size_t)extra_count * sizeof(Picture));
				if (list.count)
					memcpy(merged + extra_count, list.items, (size_t)list.count * sizeof(Picture));
				free(list.items);
				list.items = merged;
				list.count += extra_count;
			}
		}
		group_folders();
		log_write("pictures: %d found in %d folder%s", list.count, folder_count, folder_count == 1 ? "" : "s");
		folder = selected = 0;
		scroll = scroll_target = chip_scroll = 0.0f;
		pick_count = 0; // picks index the list, so they go with it
		viewer = -1;
		scanning = 0;
	} else {
		pictures_free(&job->result); // the picker was reopened meanwhile
	}
	free(job);
}

static void open_picker(const char *heading, int max, PicturePickFn done);

void picture_picker_open_with(const char *heading, int max, const char *extra_folder, const char *const *paths,
                              int count, PicturePickFn done) {
	extra_count = 0;
	for (int i = 0; i < count && extra_count < MAX_EXTRAS; i++) {
		snprintf(extras[extra_count].path, sizeof(extras[0].path), "%s", paths[i]);
		snprintf(extras[extra_count].folder, sizeof(extras[0].folder), "%s", extra_folder);
		extra_count++;
	}
	open_picker(heading, max, done);
}

void picture_picker_open(const char *heading, int max, PicturePickFn done) {
	extra_count = 0;
	open_picker(heading, max, done);
}

static void open_picker(const char *heading, int max, PicturePickFn done) {
	snprintf(title, sizeof(title), "%s", heading);
	max_pick = max < 1 ? 1 : max > MAX_PICKS ? MAX_PICKS : max;
	on_done = done;
	pick_count = 0;
	viewer = -1;
	is_open = 1;
	// Rescan every time: new screenshots or copied pictures should show up.
	// Nothing can be picked until the fresh list is in.
	pictures_free(&list);
	folder_count = folder = selected = 0;
	ScanJob *job = calloc(1, sizeof(*job));
	if (!job)
		return;
	job->generation = ++scan_generation;
	scanning = 1;
	jobs_submit(scan_work, scan_done, job);
}

int picture_picker_is_open(void) {
	return is_open;
}

static void finish(int cancelled) {
	const char *paths[MAX_PICKS];
	int n = cancelled ? 0 : pick_count;
	for (int i = 0; i < n; i++)
		paths[i] = list.items[picks[i]].path;
	is_open = 0;
	if (on_done)
		on_done(paths, n);
}

// --- input --------------------------------------------------------------------------

static int pick_number(int index) {
	for (int i = 0; i < pick_count; i++)
		if (picks[i] == index)
			return i + 1;
	return 0;
}

static void choose(int index) {
	if (max_pick == 1) {
		picks[0] = index;
		pick_count = 1;
		finish(0);
		return;
	}
	int n = pick_number(index);
	if (n) {
		memmove(picks + n - 1, picks + n, sizeof(int) * (size_t)(pick_count - n));
		pick_count--;
	} else if (pick_count == max_pick) {
		char text[96];
		snprintf(text, sizeof(text), "You can add up to %d picture%s here", max_pick, max_pick == 1 ? "" : "s");
		app_toast(text);
	} else {
		picks[pick_count++] = index;
	}
}

static void switch_folder(int f) {
	if (f < 0 || f >= folder_count || f == folder)
		return;
	folder = f;
	selected = 0;
	scroll = scroll_target = 0.0f;
}

static void keep_visible(void) {
	float top = (float)(selected / COLS) * PITCH;
	if (top - 8.0f < scroll_target)
		scroll_target = top - 8.0f;
	if (top + CARD_H + 12.0f > scroll_target + VIEW_H)
		scroll_target = top + CARD_H + 12.0f - VIEW_H;
	if (scroll_target < 0)
		scroll_target = 0;
}

void picture_picker_update(const Input *in) {
	if (viewer >= 0) {
		if (in->pressed || in->tapped)
			viewer = -1;
		return;
	}
	if (in->pressed & BTN_BACK) {
		finish(1);
		return;
	}
	int count = folder_count ? folder_len[folder] : 0, start = folder_count ? folder_start[folder] : 0;
	if (in->repeat & SCE_CTRL_LTRIGGER)
		switch_folder(folder - 1);
	if (in->repeat & SCE_CTRL_RTRIGGER)
		switch_folder(folder + 1);
	if ((in->repeat & SCE_CTRL_LEFT) && selected % COLS > 0)
		selected--;
	if ((in->repeat & SCE_CTRL_RIGHT) && selected % COLS < COLS - 1 && selected < count - 1)
		selected++;
	if ((in->repeat & SCE_CTRL_UP) && selected >= COLS)
		selected -= COLS;
	if ((in->repeat & SCE_CTRL_DOWN) && selected + COLS < count)
		selected += COLS;
	if ((in->pressed & BTN_OK) && count > 0) {
		choose(start + selected);
		if (!is_open)
			return;
	} else if ((in->pressed & SCE_CTRL_SQUARE) && count > 0) {
		viewer = start + selected;
	} else if ((in->pressed & SCE_CTRL_TRIANGLE) && max_pick > 1) {
		if (pick_count > 0) {
			finish(0);
			return;
		}
		app_toast("Pick some pictures first");
	}

	if (in->tapped) {
		int x = in->tap_x, y = in->tap_y;
		if (y >= CHIP_Y && y <= CHIP_Y + CHIP_H) {
			for (int f = 0; f < folder_count; f++)
				if (x >= chip_x[f] && x <= chip_x[f] + chip_w[f])
					switch_folder(f);
		} else if (y >= GRID_TOP && y <= GRID_BOTTOM) {
			float cy = (float)y - GRID_TOP + scroll;
			int row = (int)(cy / PITCH), col = (int)(((float)x - MARGIN) / (CARD_W + GAP));
			int i = row * COLS + col;
			if (col >= 0 && col < COLS && i < count && cy - (float)row * PITCH <= CARD_H) {
				selected = i;
				choose(start + i);
				if (!is_open)
					return;
			}
		}
	}
	if (in->dragging)
		scroll = scroll_target = scroll_target - in->drag_dy;
	else
		keep_visible();
	int rows = (count + COLS - 1) / COLS;
	float max = (float)rows * PITCH - VIEW_H;
	if (scroll_target > max)
		scroll_target = max > 0 ? max : 0;
	if (scroll_target < 0)
		scroll_target = 0;
	scroll = ui_approach(scroll, scroll_target, 16.0f);
}

// --- drawing ------------------------------------------------------------------------

static const char *file_name(const char *path) {
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static void draw_card(int index, int focused, float x, float y) {
	if (focused)
		ui_shadow(x, y, CARD_W, CARD_H, 14.0f, 1.0f);
	ui_rrect(x, y, CARD_W, CARD_H, 14.0f, focused ? C_SURFACE2 : C_SURFACE);
	ui_rrect(x, y, CARD_W, IMAGE_H, 14.0f, C_SURFACE3);
	ui_rect(x, y + IMAGE_H - 14.0f, CARD_W, 14.0f, C_SURFACE3);
	ImageRef img = images_file_sized(list.items[index].path, IMAGES_CARD_WIDTH);
	if (img.tex)
		ui_image(img.tex, x, y, CARD_W, IMAGE_H, 14, 14, 0, 0, RGBA8(255, 255, 255, (int)(255 * img.alpha)));
	char fitted[128];
	ui_text_fit(FONT_SMALL, file_name(list.items[index].path), (int)CARD_W - 20, fitted, sizeof(fitted));
	ui_text(FONT_SMALL, x + 10.0f, y + IMAGE_H + 20.0f, C_TEXT2, fitted);
	int number = pick_number(index);
	if (number) {
		float cx = x + CARD_W - 18.0f, cy = y + 18.0f;
		vita2d_draw_fill_circle(cx, cy, 13.0f, C_ACCENT);
		char text[8];
		snprintf(text, sizeof(text), "%d", number);
		ui_text_center(FONT_BOLD, cx, cy + 5.5f, C_ON_ACCENT, text);
	}
	if (focused)
		ui_rrect_outline(x - 4.0f, y - 4.0f, CARD_W + 8.0f, CARD_H + 8.0f, 18.0f, 2.5f,
		                 ui_alpha(C_ACCENT, 0.75f + 0.25f * sinf(ui_time() * 4.0f)));
}

static void draw_chips(void) {
	// Lay the chips out, then slide the row so the active one stays visible.
	float x = 0.0f;
	for (int f = 0; f < folder_count; f++) {
		char label[96];
		snprintf(label, sizeof(label), "%s  %d", folder_names[f], folder_len[f]);
		chip_w[f] = (float)ui_text_width(FONT_BOLD, label) + 32.0f;
		chip_x[f] = x;
		x += chip_w[f] + 10.0f;
	}
	float room = SCREEN_W - 2 * MARGIN, target = chip_scroll;
	if (folder_count) {
		if (chip_x[folder] < target)
			target = chip_x[folder];
		if (chip_x[folder] + chip_w[folder] > target + room)
			target = chip_x[folder] + chip_w[folder] - room;
	}
	chip_scroll = ui_approach(chip_scroll, target, 16.0f);
	vita2d_enable_clipping();
	vita2d_set_clip_rectangle((int)MARGIN - 4, (int)CHIP_Y - 4, (int)(SCREEN_W - MARGIN) + 4, (int)(CHIP_Y + CHIP_H) + 4);
	for (int f = 0; f < folder_count; f++) {
		chip_x[f] += MARGIN - chip_scroll; // screen position, for taps
		char label[96];
		snprintf(label, sizeof(label), "%s  %d", folder_names[f], folder_len[f]);
		int active = f == folder;
		ui_rrect(chip_x[f], CHIP_Y, chip_w[f], CHIP_H, CHIP_H * 0.5f, active ? ui_alpha(C_ACCENT, 0.24f) : C_SURFACE);
		ui_text(FONT_BOLD, chip_x[f] + 16.0f, CHIP_Y + 21.5f, active ? C_TEXT : C_TEXT2, label);
	}
	vita2d_disable_clipping();
}

void picture_picker_draw(void) {
	if (!is_open)
		return;
	ui_rect(0, 0, SCREEN_W, SCREEN_H, C_BG);
	ui_text(FONT_TITLE, MARGIN, 41.0f, C_TEXT, title);
	if (max_pick > 1) {
		char status[64];
		snprintf(status, sizeof(status), "%d of %d picked", pick_count, max_pick);
		ui_text_right(FONT_BODY, SCREEN_W - MARGIN, 40.0f, pick_count ? C_TEXT : C_TEXT3, status);
	}

	if (scanning && list.count == 0) {
		ui_spinner(SCREEN_W * 0.5f, GRID_TOP + 120.0f, 14.0f, C_ACCENT);
	} else if (folder_count == 0) {
		ui_text_center(FONT_TITLE, SCREEN_W * 0.5f, GRID_TOP + 110.0f, C_TEXT, "No pictures yet");
		ui_text_center(FONT_BODY, SCREEN_W * 0.5f, GRID_TOP + 140.0f, C_TEXT2,
		               "Take screenshots, or copy JPEG or PNG pictures to " PICTURES_DIR);
	} else {
		draw_chips();
		int count = folder_len[folder], start = folder_start[folder];
		vita2d_enable_clipping();
		vita2d_set_clip_rectangle(0, (int)GRID_TOP - 10, SCREEN_W, (int)GRID_BOTTOM + 4);
		int first = (int)floorf((scroll - 20.0f) / PITCH), last = (int)ceilf((scroll + VIEW_H) / PITCH);
		for (int row = first < 0 ? 0 : first; row <= last; row++)
			for (int col = 0; col < COLS; col++) {
				int i = row * COLS + col;
				if (i < count)
					draw_card(start + i, i == selected, MARGIN + (float)col * (CARD_W + GAP),
					          GRID_TOP + (float)row * PITCH - scroll);
			}
		vita2d_disable_clipping();
	}

	if (viewer >= 0) {
		ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 235));
		ImageRef img = images_file(list.items[viewer].path);
		if (img.tex)
			ui_image(img.tex, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 0, RGBA8(255, 255, 255, (int)(255 * img.alpha)));
		else if (!img.failed)
			ui_spinner(SCREEN_W * 0.5f, SCREEN_H * 0.5f, 16.0f, C_ACCENT);
		else
			ui_text_center(FONT_BODY, SCREEN_W * 0.5f, SCREEN_H * 0.5f, C_TEXT2, "This picture can't be shown");
		Hint hints[] = {{ui_back_glyph(), "Close"}};
		app_footer(hints, 1, NULL);
		return;
	}
	Hint hints[5];
	int n = 0;
	hints[n++] = (Hint){ui_back_glyph(), "Cancel"};
	if (folder_count > 1)
		hints[n++] = (Hint){GLYPH_R, "Folder"};
	hints[n++] = (Hint){GLYPH_SQUARE, "Preview"};
	if (max_pick > 1)
		hints[n++] = (Hint){GLYPH_TRIANGLE, "Use picked"};
	hints[n++] = (Hint){ui_ok_glyph(), max_pick > 1 ? "Pick" : "Choose"};
	app_footer(hints, n, NULL);
}
