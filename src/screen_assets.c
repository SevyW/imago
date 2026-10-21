// Assets: every wallpaper, icon set, song and lock screen of the installed
// themes, with previews, to build a brand-new theme from.
#include "app.h"
#include "assets.h"
#include "images.h"
#include "jobs.h"
#include "music.h"
#include "themedoc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLS 4
#define GAP 16.0f
#define CARD_W ((SCREEN_W - 2 * MARGIN - (COLS - 1) * GAP) / COLS)
#define IMAGE_H ((float)(int)(CARD_W * 544.0f / 960.0f))
#define CARD_H (IMAGE_H + 34.0f)
#define PITCH (CARD_H + 16.0f)
#define CHIP_Y (HEADER_H + 6.0f)
#define CHIP_H 32.0f
#define GRID_TOP (HEADER_H + 54.0f)
#define BAR_Y (FOOTER_Y - 50.0f)
#define GRID_BOTTOM (BAR_Y - 8.0f)
#define VIEW_H (GRID_BOTTOM - GRID_TOP)
#define MAX_WALLPAPERS 10

static const char *const kind_names[ASSET_KIND_COUNT] = {"Wallpapers", "Icons", "Music", "Lock screens"};

static AssetIndex index_;
static int scanning, scanned_generation = -1;
static AssetKind kind;
static int selected[ASSET_KIND_COUNT];
static float scroll, scroll_target;
static int on_chips;
static int picked_pages[MAX_WALLPAPERS], picked_page_count; // asset indices, in pick order
static int picked[ASSET_KIND_COUNT] = {-1, -1, -1, -1};    // icons / music / lock: asset index or -1
static int viewer = -1;                                     // asset shown full screen
static float chip_x[ASSET_KIND_COUNT], chip_w[ASSET_KIND_COUNT];

// --- index ---------------------------------------------------------------------------

typedef struct {
	LibraryItem *items;
	int count;
	int generation;
	AssetIndex result;
} ScanJob;

static void scan_work(void *ctx, NetClient *net) {
	(void)net;
	ScanJob *job = ctx;
	assets_scan(job->items, job->count, &job->result);
}

static void clear_picks(void) {
	picked_page_count = 0;
	for (int k = 0; k < ASSET_KIND_COUNT; k++)
		picked[k] = -1;
}

static void scan_done(void *ctx) {
	ScanJob *job = ctx;
	assets_free(&index_);
	index_ = job->result;
	scanned_generation = job->generation;
	scanning = 0;
	clear_picks(); // indices refer to the old index
	for (int k = 0; k < ASSET_KIND_COUNT; k++)
		selected[k] = 0;
	free(job->items);
	free(job);
}

static void ensure_index(void) {
	if (scanning || scanned_generation == g_library_generation || g_library_loading)
		return;
	ScanJob *job = calloc(1, sizeof(*job));
	if (!job)
		return;
	job->items = malloc((size_t)(g_library.count ? g_library.count : 1) * sizeof(LibraryItem));
	if (!job->items) {
		free(job);
		return;
	}
	memcpy(job->items, g_library.items, (size_t)g_library.count * sizeof(LibraryItem));
	job->count = g_library.count;
	job->generation = g_library_generation;
	scanning = 1;
	jobs_submit(scan_work, scan_done, job);
}

static int kind_start(AssetKind k) {
	int start = 0;
	for (int i = 0; i < (int)k; i++)
		start += index_.per_kind[i];
	return start;
}

static int kind_count(AssetKind k) {
	return index_.per_kind[k];
}

// --- picking and building ---------------------------------------------------------------

static int page_pick_number(int asset) {
	for (int i = 0; i < picked_page_count; i++)
		if (picked_pages[i] == asset)
			return i + 1;
	return 0;
}

static void toggle_pick(int asset) {
	if (kind == ASSET_WALLPAPERS) {
		int n = page_pick_number(asset);
		if (n) {
			memmove(picked_pages + n - 1, picked_pages + n, sizeof(int) * (size_t)(picked_page_count - n));
			picked_page_count--;
		} else if (picked_page_count == MAX_WALLPAPERS) {
			app_toast("A theme can have up to 10 wallpapers");
		} else {
			picked_pages[picked_page_count++] = asset;
		}
	} else {
		picked[kind] = picked[kind] == asset ? -1 : asset;
	}
}

static int total_picks(void) {
	int n = picked_page_count;
	for (int k = 1; k < ASSET_KIND_COUNT; k++)
		n += picked[k] >= 0;
	return n;
}

static int load_theme(ThemeDoc *docs, int *loaded, int theme) {
	if (!loaded[theme]) {
		if (themedoc_load(&docs[theme], index_.themes[theme].dir) != 0)
			return -1;
		loaded[theme] = 1;
	}
	return 0;
}

// Builds a theme from the picks: the first pick's theme supplies everything
// not picked (status bar, page dots...), and each pick is layered on top.
static void create_theme(void) {
	int base_asset = picked_page_count ? picked_pages[0]
	                 : picked[ASSET_LOCKSCREENS] >= 0 ? picked[ASSET_LOCKSCREENS]
	                 : picked[ASSET_ICONS] >= 0       ? picked[ASSET_ICONS]
	                                                  : picked[ASSET_MUSIC];
	if (base_asset < 0) {
		app_toast("Pick a few wallpapers, icons, music or a lock screen first");
		return;
	}
	int n = index_.theme_count;
	ThemeDoc *docs = calloc((size_t)n, sizeof(ThemeDoc));
	int *loaded = calloc((size_t)n, sizeof(int));
	ThemeDoc result;
	int ok = docs && loaded && load_theme(docs, loaded, index_.items[base_asset].theme) == 0 &&
	         themedoc_copy(&result, &docs[index_.items[base_asset].theme]) == 0;
	if (ok && picked_page_count) {
		const ThemeDoc *froms[MAX_WALLPAPERS];
		int pages[MAX_WALLPAPERS];
		for (int i = 0; ok && i < picked_page_count; i++) {
			const Asset *a = &index_.items[picked_pages[i]];
			ok = load_theme(docs, loaded, a->theme) == 0;
			froms[i] = &docs[a->theme];
			pages[i] = a->page;
		}
		ok = ok && themedoc_set_pages(&result, froms, pages, picked_page_count) == 0;
	}
	static const ThemePart parts[ASSET_KIND_COUNT] = {PART_WALLPAPERS, PART_ICONS, PART_MUSIC, PART_LOCKSCREEN};
	for (int k = 1; ok && k < ASSET_KIND_COUNT; k++) {
		if (picked[k] < 0)
			continue;
		int theme = index_.items[picked[k]].theme;
		ok = load_theme(docs, loaded, theme) == 0 && themedoc_take_part(&result, parts[k], &docs[theme]) == 0;
	}
	for (int i = 0; i < n && loaded; i++)
		if (loaded[i])
			themedoc_free(&docs[i]);
	free(docs);
	free(loaded);
	if (!ok) {
		app_toast("Couldn't put that theme together");
		return;
	}
	music_stop();
	studio_open_doc(&result, "My theme", 1);
}

// --- input ---------------------------------------------------------------------------

static void keep_visible(void) {
	float top = (float)(selected[kind] / COLS) * PITCH;
	if (top - 8.0f < scroll_target)
		scroll_target = top - 8.0f;
	if (top + CARD_H + 12.0f > scroll_target + VIEW_H)
		scroll_target = top + CARD_H + 12.0f - VIEW_H;
	if (scroll_target < 0)
		scroll_target = 0;
}

static void switch_kind(AssetKind k) {
	if (k == kind)
		return;
	kind = k;
	scroll = scroll_target = 0;
	keep_visible();
}

static void preview(int asset) {
	const Asset *a = &index_.items[asset];
	if (a->kind == ASSET_MUSIC) {
		if (music_state(a->file) == MUSIC_IDLE || music_state(a->file) == MUSIC_ERROR)
			music_play(a->file);
		else
			music_stop();
	} else {
		viewer = asset;
	}
}

void assets_screen_leave(void) {
	music_stop();
	viewer = -1;
}

void assets_screen_update(const Input *in) {
	ensure_index();
	if (viewer >= 0) {
		if (in->pressed & (BTN_BACK | BTN_OK | SCE_CTRL_SQUARE) || in->tapped)
			viewer = -1;
		return;
	}
	int count = kind_count(kind);
	int *sel = &selected[kind];
	if (*sel >= count)
		*sel = count > 0 ? count - 1 : 0;

	if (on_chips) {
		if (in->repeat & SCE_CTRL_LEFT)
			switch_kind((AssetKind)((kind + ASSET_KIND_COUNT - 1) % ASSET_KIND_COUNT));
		if (in->repeat & SCE_CTRL_RIGHT)
			switch_kind((AssetKind)((kind + 1) % ASSET_KIND_COUNT));
		if (in->repeat & (SCE_CTRL_DOWN | BTN_OK))
			on_chips = 0;
	} else {
		if ((in->repeat & SCE_CTRL_LEFT) && *sel % COLS > 0)
			(*sel)--;
		if ((in->repeat & SCE_CTRL_RIGHT) && *sel % COLS < COLS - 1 && *sel < count - 1)
			(*sel)++;
		if (in->repeat & SCE_CTRL_UP) {
			if (*sel < COLS)
				on_chips = 1;
			else
				*sel -= COLS;
		}
		if ((in->repeat & SCE_CTRL_DOWN) && *sel + COLS < count)
			*sel += COLS;
		if ((in->pressed & BTN_OK) && count > 0)
			toggle_pick(kind_start(kind) + *sel);
		else if ((in->pressed & SCE_CTRL_SQUARE) && count > 0)
			preview(kind_start(kind) + *sel);
	}
	if (in->pressed & SCE_CTRL_TRIANGLE)
		create_theme();
	else if (in->pressed & SCE_CTRL_SELECT)
		clear_picks();

	if (in->tapped) {
		int x = in->tap_x, y = in->tap_y;
		if (y >= CHIP_Y && y <= CHIP_Y + CHIP_H) {
			for (int k = 0; k < ASSET_KIND_COUNT; k++)
				if (x >= chip_x[k] && x <= chip_x[k] + chip_w[k])
					switch_kind((AssetKind)k);
		} else if (y >= GRID_TOP && y <= GRID_BOTTOM) {
			float cy = (float)y - GRID_TOP + scroll;
			int row = (int)(cy / PITCH), col = (int)(((float)x - MARGIN) / (CARD_W + GAP));
			int i = row * COLS + col;
			if (col >= 0 && col < COLS && i < count && cy - (float)row * PITCH <= CARD_H) {
				*sel = i;
				on_chips = 0;
				toggle_pick(kind_start(kind) + i);
			}
		} else if (y >= BAR_Y && y <= BAR_Y + 42.0f && x > SCREEN_W - 260) {
			create_theme();
		}
	}
	if (in->dragging)
		scroll = scroll_target = scroll_target - in->drag_dy;
	else if (!on_chips)
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

static void draw_image(const char *path, float x, float y, float w, float h, float r_bottom) {
	ImageRef img = images_file(path);
	if (img.tex)
		ui_image(img.tex, x, y, w, h, 14, 14, r_bottom, r_bottom, RGBA8(255, 255, 255, (int)(255 * img.alpha)));
}

static void draw_badge_number(float cx, float cy, int number) {
	vita2d_draw_fill_circle(cx, cy, 13.0f, C_ACCENT);
	char text[8];
	snprintf(text, sizeof(text), "%d", number);
	ui_text_center(FONT_BOLD, cx, cy + 5.5f, C_ON_ACCENT, text);
}

static void draw_check(float cx, float cy) {
	vita2d_draw_fill_circle(cx, cy, 13.0f, C_ACCENT);
	ui_line(cx - 5.5f, cy, cx - 1.5f, cy + 4.5f, 2.4f, C_ON_ACCENT);
	ui_line(cx - 1.5f, cy + 4.5f, cx + 6.0f, cy - 4.5f, 2.4f, C_ON_ACCENT);
}

static void draw_card(int i, float x, float y) {
	int asset = kind_start(kind) + i;
	const Asset *a = &index_.items[asset];
	const AssetTheme *t = &index_.themes[a->theme];
	int focused = !on_chips && selected[kind] == i;
	if (focused)
		ui_shadow(x, y, CARD_W, CARD_H, 14.0f, 1.0f);
	ui_rrect(x, y, CARD_W, CARD_H, 14.0f, focused ? C_SURFACE2 : C_SURFACE);
	ui_rrect(x, y, CARD_W, IMAGE_H, 14.0f, C_SURFACE3);
	ui_rect(x, y + IMAGE_H - 14.0f, CARD_W, 14.0f, C_SURFACE3);

	if (a->kind == ASSET_ICONS) {
		// A 4x2 grid of the set's first icons.
		float s = 44.0f, gx = x + (CARD_W - 4 * s - 3 * 8.0f) * 0.5f, gy = y + (IMAGE_H - 2 * s - 8.0f) * 0.5f;
		int n = 0;
		for (int k = 0; k < ASSET_PREVIEW_ICONS && t->icons[k][0]; k++, n++) {
			ImageRef img = images_file(t->icons[k]);
			float ix = gx + (float)(k % 4) * (s + 8.0f), iy = gy + (float)(k / 4) * (s + 8.0f);
			if (img.tex)
				ui_image(img.tex, ix, iy, s, s, 10, 10, 10, 10, RGBA8(255, 255, 255, (int)(255 * img.alpha)));
		}
	} else {
		draw_image(a->image, x, y, CARD_W, IMAGE_H, 0);
	}
	if (a->kind == ASSET_MUSIC) {
		MusicState st = music_state(a->file);
		float cx = x + CARD_W * 0.5f, cy = y + IMAGE_H * 0.5f;
		vita2d_draw_fill_circle(cx, cy, 26.0f, RGBA8(0, 0, 0, 160));
		if (st == MUSIC_LOADING) {
			ui_spinner(cx, cy, 13.0f, C_TEXT);
		} else if (st == MUSIC_PLAYING) { // pause bars
			ui_rect(cx - 8.0f, cy - 10.0f, 6.0f, 20.0f, C_TEXT);
			ui_rect(cx + 2.0f, cy - 10.0f, 6.0f, 20.0f, C_TEXT);
		} else { // play triangle
			ui_line(cx - 6.0f, cy - 11.0f, cx - 6.0f, cy + 11.0f, 3.0f, C_TEXT);
			ui_line(cx - 6.0f, cy - 11.0f, cx + 11.0f, cy, 3.0f, C_TEXT);
			ui_line(cx + 11.0f, cy, cx - 6.0f, cy + 11.0f, 3.0f, C_TEXT);
		}
	}

	char label[160], fitted[160];
	if (a->kind == ASSET_WALLPAPERS)
		snprintf(label, sizeof(label), "%s \xC2\xB7 %d", t->title, a->page + 1);
	else if (a->kind == ASSET_ICONS)
		snprintf(label, sizeof(label), "%s \xC2\xB7 %d icons", t->title, t->icon_count);
	else
		snprintf(label, sizeof(label), "%s", t->title);
	ui_text_fit(FONT_SMALL, label, (int)CARD_W - 20, fitted, sizeof(fitted));
	ui_text(FONT_SMALL, x + 10.0f, y + IMAGE_H + 22.0f, C_TEXT2, fitted);

	if (a->kind == ASSET_WALLPAPERS) {
		int number = page_pick_number(asset);
		if (number)
			draw_badge_number(x + CARD_W - 18.0f, y + 18.0f, number);
	} else if (picked[a->kind] == asset) {
		draw_check(x + CARD_W - 18.0f, y + 18.0f);
	}
	if (focused)
		ui_rrect_outline(x - 4.0f, y - 4.0f, CARD_W + 8.0f, CARD_H + 8.0f, 18.0f, 2.5f,
		                 ui_alpha(C_ACCENT, 0.75f + 0.25f * sinf(ui_time() * 4.0f)));
}

static void draw_viewer(void) {
	const Asset *a = &index_.items[viewer];
	const AssetTheme *t = &index_.themes[a->theme];
	ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 235));
	if (a->kind == ASSET_ICONS) {
		ui_text_center(FONT_TITLE, SCREEN_W * 0.5f, 70.0f, C_TEXT, t->title);
		float s = 112.0f, gx = (SCREEN_W - 4 * s - 3 * 24.0f) * 0.5f;
		for (int k = 0; k < ASSET_PREVIEW_ICONS && t->icons[k][0]; k++) {
			ImageRef img = images_file(t->icons[k]);
			float ix = gx + (float)(k % 4) * (s + 24.0f), iy = 110.0f + (float)(k / 4) * (s + 30.0f);
			if (img.tex)
				ui_image(img.tex, ix, iy, s, s, 20, 20, 20, 20, RGBA8(255, 255, 255, (int)(255 * img.alpha)));
		}
		char more[64];
		snprintf(more, sizeof(more), "%d custom icons in this set", t->icon_count);
		ui_text_center(FONT_BODY, SCREEN_W * 0.5f, 420.0f, C_TEXT2, more);
	} else {
		ImageRef img = images_file(a->file);
		if (img.tex)
			ui_image(img.tex, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 0, RGBA8(255, 255, 255, (int)(255 * img.alpha)));
		else
			ui_spinner(SCREEN_W * 0.5f, SCREEN_H * 0.5f, 16.0f, C_ACCENT);
		char label[200];
		snprintf(label, sizeof(label), "%s \xC2\xB7 %s", t->title, t->provider);
		int w = ui_text_width(FONT_BODY, label);
		ui_rrect(20.0f, 20.0f, (float)w + 28.0f, 34.0f, 17.0f, RGBA8(0, 0, 0, 170));
		ui_text(FONT_BODY, 34.0f, 42.0f, C_TEXT, label);
	}
}

void assets_screen_draw(void) {
	// Kind chips
	float x = MARGIN;
	for (int k = 0; k < ASSET_KIND_COUNT; k++) {
		char label[48];
		snprintf(label, sizeof(label), "%s  %d", kind_names[k], index_.per_kind[k]);
		float w = (float)ui_text_width(FONT_BOLD, label) + 32.0f;
		int active = k == (int)kind;
		ui_rrect(x, CHIP_Y, w, CHIP_H, CHIP_H * 0.5f, active ? ui_alpha(C_ACCENT, 0.24f) : C_SURFACE);
		ui_text(FONT_BOLD, x + 16.0f, CHIP_Y + 21.5f, active ? C_TEXT : C_TEXT2, label);
		if (on_chips && active)
			ui_rrect_outline(x - 3.0f, CHIP_Y - 3.0f, w + 6.0f, CHIP_H + 6.0f, CHIP_H * 0.5f + 3.0f, 2.0f, C_ACCENT);
		chip_x[k] = x;
		chip_w[k] = w;
		x += w + 10.0f;
	}

	int count = kind_count(kind);
	if (scanning && index_.count == 0) {
		ui_spinner(SCREEN_W * 0.5f, GRID_TOP + 120.0f, 14.0f, C_ACCENT);
	} else if (count == 0) {
		ui_text_center(FONT_TITLE, SCREEN_W * 0.5f, GRID_TOP + 110.0f, C_TEXT,
		               index_.theme_count ? "Nothing here yet" : "No themes installed yet");
		ui_text_center(FONT_BODY, SCREEN_W * 0.5f, GRID_TOP + 140.0f, C_TEXT2,
		               "Install themes from Discover and their parts will show up here.");
	} else {
		vita2d_enable_clipping();
		vita2d_set_clip_rectangle(0, (int)GRID_TOP - 10, SCREEN_W, (int)GRID_BOTTOM + 4);
		int first = (int)floorf((scroll - 20.0f) / PITCH), last = (int)ceilf((scroll + VIEW_H) / PITCH);
		for (int row = first < 0 ? 0 : first; row <= last; row++)
			for (int col = 0; col < COLS; col++) {
				int i = row * COLS + col;
				if (i < count)
					draw_card(i, MARGIN + (float)col * (CARD_W + GAP), GRID_TOP + (float)row * PITCH - scroll);
			}
		vita2d_disable_clipping();
	}

	// Picks summary and the create button
	ui_rrect(MARGIN, BAR_Y, SCREEN_W - 2 * MARGIN, 42.0f, 14.0f, C_SURFACE);
	char summary[256], part[96];
	snprintf(summary, sizeof(summary), "%d wallpaper%s", picked_page_count, picked_page_count == 1 ? "" : "s");
	static const char *const short_names[ASSET_KIND_COUNT] = {"", "icons", "music", "lock screen"};
	for (int k = 1; k < ASSET_KIND_COUNT; k++) {
		if (picked[k] < 0)
			continue;
		snprintf(part, sizeof(part), " \xC2\xB7 %s: %s", short_names[k], index_.themes[index_.items[picked[k]].theme].title);
		strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
	}
	char fitted[256];
	ui_text_fit(FONT_BODY, total_picks() ? summary : "Pick parts with \xE2\x9C\x95 to build a new theme", 600,
	            fitted, sizeof(fitted));
	ui_text(FONT_BODY, MARGIN + 16.0f, BAR_Y + 27.0f, total_picks() ? C_TEXT : C_TEXT3, fitted);
	float bw = 200.0f, bx = SCREEN_W - MARGIN - bw - 5.0f;
	ui_rrect(bx, BAR_Y + 5.0f, bw, 32.0f, 11.0f, total_picks() ? C_ACCENT : C_SURFACE3);
	ui_glyph(GLYPH_TRIANGLE, bx + 20.0f, BAR_Y + 21.0f, 1.0f);
	ui_text(FONT_BOLD, bx + 38.0f, BAR_Y + 27.0f, total_picks() ? C_ON_ACCENT : C_TEXT3, "Create theme");

	if (viewer >= 0) {
		draw_viewer();
		Hint hints[] = {{ui_back_glyph(), "Close"}};
		app_footer(hints, 1, NULL);
		return;
	}
	Hint hints[4];
	int n = 0;
	if (total_picks())
		hints[n++] = (Hint){GLYPH_SELECT, "Clear picks"};
	hints[n++] = (Hint){GLYPH_SQUARE, kind == ASSET_MUSIC ? "Play / stop" : "Preview"};
	hints[n++] = (Hint){ui_ok_glyph(), "Pick"};
	app_footer(hints, n, NULL);
}
