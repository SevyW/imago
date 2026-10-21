// Theme Studio: customise an installed theme, mix in parts of other themes,
// and save the result as a new theme.
#include "app.h"
#include "dialog.h"
#include "images.h"
#include "ime.h"
#include "picture_picker.h"
#include "tasks.h"
#include "themedoc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PREVIEW_X 32.0f
#define PREVIEW_Y 72.0f
#define PREVIEW_W 560.0f
#define PREVIEW_H 299.0f
#define STRIP_Y (PREVIEW_Y + PREVIEW_H + 16.0f)
#define THUMB_W 104.0f
#define THUMB_H 56.0f
#define THUMB_GAP 8.0f
#define COL_X 620.0f
#define COL_W (SCREEN_W - COL_X - 32.0f)
#define ROW_Y 70.0f
#define ROW_H 50.0f
#define ROW_GAP 6.0f
#define SAVE_Y 412.0f
#define SAVE_H 46.0f

// Picker (choose another theme to take a part from)
#define PICK_W 620.0f
#define PICK_H 400.0f
#define PICK_ROW_H 58.0f
#define PICK_ROWS 5

enum { ROW_NAME, ROW_WALLPAPERS, ROW_MUSIC, ROW_ICONS, ROW_LOCKSCREEN, ROW_STATUSBAR, ROW_SAVE, ROW_COUNT };

static int is_open, closing;
static float appear;
static ThemeDoc work;
static char base_dir[256];
static char base_title[128];
static char name[128];
static int page_count;
static int page_keep[THEMEDOC_MAX_PAGES];
static int music_on, icons_on;
static int focus;
static int strip_active, strip_focus;
static int is_new; // built from the asset library: always redraw previews
static int previews_dirty;   // pictures were added: the previews must be redrawn
static int pictures_in_pages; // how many pages are your own pictures
static int lock_from_picture; // the lock screen is one of your pictures
static float strip_scroll;

static int picking; // the picker is open for picker_part
static ThemePart picker_part;
static int picker_focus;
static float picker_scroll;

int studio_is_open(void) {
	return is_open;
}

static void reset_pages(void) {
	page_count = themedoc_page_count(&work);
	for (int i = 0; i < THEMEDOC_MAX_PAGES; i++)
		page_keep[i] = 1;
	strip_focus = 0;
	strip_scroll = 0.0f;
}

int studio_open_doc(void *doc_ptr, const char *new_name, int new_theme) {
	ThemeDoc *doc = doc_ptr;
	if (is_open)
		themedoc_free(&work);
	work = *doc;
	memset(doc, 0, sizeof(*doc)); // ownership moved here
	is_new = new_theme;
	previews_dirty = pictures_in_pages = lock_from_picture = 0;
	snprintf(base_dir, sizeof(base_dir), "%s", work.origins[0]);
	if (new_theme)
		snprintf(base_title, sizeof(base_title), "New theme");
	else
		themedoc_get_title(&work, base_title, sizeof(base_title));
	snprintf(name, sizeof(name), "%s", new_name);
	reset_pages();
	music_on = themedoc_has_part(&work, PART_MUSIC);
	icons_on = themedoc_has_part(&work, PART_ICONS);
	focus = ROW_NAME;
	strip_active = 0;
	picking = 0;
	is_open = 1;
	closing = 0;
	appear = 0.0f;
	return 0;
}

int studio_open(const char *dir) {
	ThemeDoc doc;
	if (themedoc_load(&doc, dir) != 0) {
		dialog_message("Can't open this theme", "Its theme.xml is missing or unreadable.");
		return -1;
	}
	char title[128], new_name[160];
	themedoc_get_title(&doc, title, sizeof(title));
	snprintf(new_name, sizeof(new_name), "%s \xC2\xB7 custom", title[0] ? title : "Theme");
	return studio_open_doc(&doc, new_name, 0);
}

static void close_studio(void) {
	closing = 1;
}

static int kept_pages(void) {
	int n = 0;
	for (int i = 0; i < page_count; i++)
		n += page_keep[i];
	return n;
}

static int first_kept(void) {
	for (int i = 0; i < page_count; i++)
		if (page_keep[i])
			return i;
	return 0;
}

static void toggle_page(int i) {
	if (page_keep[i] && kept_pages() == 1) {
		app_toast("A theme needs at least one wallpaper");
		return;
	}
	page_keep[i] = !page_keep[i];
}

static ThemePart row_part(int row) {
	switch (row) {
	case ROW_WALLPAPERS: return PART_WALLPAPERS;
	case ROW_MUSIC: return PART_MUSIC;
	case ROW_ICONS: return PART_ICONS;
	case ROW_LOCKSCREEN: return PART_LOCKSCREEN;
	case ROW_STATUSBAR: return PART_STATUSBAR;
	default: return PART_COUNT;
	}
}

// Parts not taken from the base theme, or edited, need new previews.
static int changes_previews(void) {
	return is_new || previews_dirty || kept_pages() < page_count || themedoc_part_origin(&work, PART_WALLPAPERS) > 0 ||
	       themedoc_part_origin(&work, PART_LOCKSCREEN) > 0;
}

static void save(void) {
	if (!name[0]) {
		app_toast("Give the new theme a name first");
		return;
	}
	if (!g_db_writable) {
		dialog_message("Unsafe homebrew is disabled",
		               "Saving a theme needs write access to the system theme list. Enable \"Unsafe homebrew\" "
		               "in HENkaku Settings, then restart Imago.");
		return;
	}
	ThemeDoc *out = malloc(sizeof(*out));
	if (!out || themedoc_copy(out, &work) != 0) {
		free(out);
		app_toast("Not enough memory to save this theme");
		return;
	}
	if (!music_on)
		themedoc_remove_part(out, PART_MUSIC);
	if (!icons_on)
		themedoc_remove_part(out, PART_ICONS);
	if (kept_pages() < page_count)
		themedoc_keep_pages(out, page_keep, page_count);
	themedoc_set_title(out, name);
	char credit[64];
	themedoc_credit(out, credit, sizeof(credit));
	if (credit[0])
		themedoc_set_provider(out, credit); // credit every author whose parts are used
	tasks_build(out, name, changes_previews());
	char text[200];
	snprintf(text, sizeof(text), "Saving \"%s\"...", name);
	app_toast(text);
	close_studio();
}

// --- picker -------------------------------------------------------------------------

// Wallpapers and the lock screen can also come from your own pictures, offered
// as the first row of the picker.
static int pictures_row(void) {
	return picker_part == PART_WALLPAPERS || picker_part == PART_LOCKSCREEN;
}

static int picker_count(void) {
	return g_library.count + pictures_row();
}

static const LibraryItem *picker_item(int i) {
	i -= pictures_row();
	return i >= 0 ? &g_library.items[i] : NULL;
}

static int picker_usable(int i) {
	const LibraryItem *it = picker_item(i);
	return !it || it->has_files;
}

static void open_picker(ThemePart part) {
	int offers_pictures = part == PART_WALLPAPERS || part == PART_LOCKSCREEN;
	if (g_library.count == 0 && !offers_pictures) {
		app_toast("Install a few more themes to mix parts from them");
		return;
	}
	picking = 1;
	picker_part = part;
	picker_focus = 0;
	picker_scroll = 0.0f;
}

// New pages from pictures go after the kept pages.
static void add_picture_pages(const char *const *paths, int count) {
	if (count == 0)
		return;
	ThemeDoc pics;
	if (themedoc_from_pictures(&pics, paths, count, NULL) != 0) {
		app_toast("Couldn't use those pictures");
		return;
	}
	if (kept_pages() < page_count) {
		themedoc_keep_pages(&work, page_keep, page_count);
		reset_pages(); // the work doc changed, even if adding fails below
	}
	int kept = themedoc_page_count(&work), total = kept + count;
	const ThemeDoc *froms[THEMEDOC_MAX_PAGES];
	int pages[THEMEDOC_MAX_PAGES];
	for (int i = 0; i < total && i < THEMEDOC_MAX_PAGES; i++) {
		froms[i] = i < kept ? &work : &pics;
		pages[i] = i < kept ? i : i - kept;
	}
	if (total > THEMEDOC_MAX_PAGES || themedoc_set_pages(&work, froms, pages, total) != 0) {
		app_toast("Couldn't add those pictures (too many themes mixed?)");
	} else {
		reset_pages();
		pictures_in_pages += count;
		previews_dirty = 1;
		strip_active = 1;
		strip_focus = kept;
	}
	themedoc_free(&pics);
}

static void use_lock_picture(const char *const *paths, int count) {
	if (count == 0)
		return;
	ThemeDoc pics;
	if (themedoc_from_pictures(&pics, NULL, 0, paths[0]) != 0 || themedoc_take_lockscreen_image(&work, &pics) != 0) {
		app_toast("Couldn't use that picture");
	} else {
		lock_from_picture = 1;
		previews_dirty = 1;
		focus = ROW_LOCKSCREEN;
	}
	themedoc_free(&pics);
}

static void take_from_pictures(void) {
	picking = 0;
	if (picker_part == PART_LOCKSCREEN) {
		picture_picker_open("Choose a lock screen picture", 1, use_lock_picture);
		return;
	}
	int room = THEMEDOC_MAX_PAGES - kept_pages();
	if (room <= 0) {
		app_toast("A theme can have up to 10 wallpapers. Turn one off first.");
		return;
	}
	picture_picker_open("Add your pictures as wallpapers", room, add_picture_pages);
}

static void take_from(int index) {
	if (!picker_item(index)) {
		take_from_pictures();
		return;
	}
	const LibraryItem *it = picker_item(index);
	ThemeDoc other;
	if (themedoc_load(&other, it->dir) != 0) {
		app_toast("Couldn't read that theme");
		return;
	}
	if (!themedoc_has_part(&other, picker_part)) {
		char text[160];
		snprintf(text, sizeof(text), "\"%s\" has no %s", it->title, themedoc_part_name(picker_part));
		app_toast(text);
	} else if (themedoc_take_part(&work, picker_part, &other) != 0) {
		app_toast("Couldn't take that part (too many themes mixed?)");
	} else {
		if (picker_part == PART_WALLPAPERS) {
			reset_pages();
			pictures_in_pages = 0;
		}
		if (picker_part == PART_LOCKSCREEN)
			lock_from_picture = 0;
		if (picker_part == PART_MUSIC)
			music_on = 1;
		if (picker_part == PART_ICONS)
			icons_on = 1;
		picking = 0;
	}
	themedoc_free(&other);
}

static void update_picker(const Input *in) {
	int n = picker_count();
	if ((in->repeat & SCE_CTRL_UP) && picker_focus > 0)
		picker_focus--;
	if ((in->repeat & SCE_CTRL_DOWN) && picker_focus < n - 1)
		picker_focus++;
	if ((in->pressed & BTN_OK) && n > 0 && picker_usable(picker_focus))
		take_from(picker_focus);
	else if (in->pressed & (BTN_BACK | SCE_CTRL_TRIANGLE))
		picking = 0;
	float px = (SCREEN_W - PICK_W) * 0.5f, py = (SCREEN_H - PICK_H) * 0.5f;
	float list_y = py + 64.0f;
	if (in->tapped) {
		int x = in->tap_x, y = in->tap_y;
		if (x < px || x > px + PICK_W || y < py || y > py + PICK_H) {
			picking = 0;
		} else if (y >= list_y) {
			int i = (int)(((float)y - list_y + picker_scroll) / PICK_ROW_H);
			if (i >= 0 && i < n && picker_usable(i)) {
				picker_focus = i;
				take_from(i);
			}
		}
	}
	float target = picker_scroll, fy = (float)picker_focus * PICK_ROW_H;
	if (fy < target)
		target = fy;
	if (fy + PICK_ROW_H > target + PICK_ROWS * PICK_ROW_H)
		target = fy + PICK_ROW_H - PICK_ROWS * PICK_ROW_H;
	picker_scroll = ui_approach(picker_scroll, target, 16.0f);
}

// --- input --------------------------------------------------------------------------

static void activate(int row) {
	switch (row) {
	case ROW_NAME: ime_open("Name of the new theme", name, 60); break;
	case ROW_WALLPAPERS:
		strip_active = 1;
		strip_focus = first_kept();
		break;
	case ROW_MUSIC:
		if (themedoc_has_part(&work, PART_MUSIC))
			music_on = !music_on;
		else
			open_picker(PART_MUSIC);
		break;
	case ROW_ICONS:
		if (themedoc_has_part(&work, PART_ICONS))
			icons_on = !icons_on;
		else
			open_picker(PART_ICONS);
		break;
	case ROW_LOCKSCREEN: open_picker(PART_LOCKSCREEN); break;
	case ROW_STATUSBAR: open_picker(PART_STATUSBAR); break;
	case ROW_SAVE: save(); break;
	}
}

static float row_y(int row) {
	return ROW_Y + (float)row * (ROW_H + ROW_GAP);
}

static float strip_x(int i) {
	return PREVIEW_X + (float)i * (THUMB_W + THUMB_GAP) - strip_scroll;
}

void studio_update(const Input *in) {
	if (closing) {
		appear = ui_approach(appear, 0.0f, 20.0f);
		if (appear < 0.02f) {
			is_open = 0;
			themedoc_free(&work);
		}
		return;
	}
	appear = ui_approach(appear, 1.0f, 16.0f);

	if (ime_active()) {
		char text[128];
		if (ime_poll(text, sizeof(text)) == 1 && text[0])
			snprintf(name, sizeof(name), "%s", text);
		return;
	}
	if (picture_picker_is_open()) {
		picture_picker_update(in);
		return;
	}
	if (picking) {
		update_picker(in);
		return;
	}

	if (strip_active) {
		if ((in->repeat & SCE_CTRL_LEFT) && strip_focus > 0)
			strip_focus--;
		if ((in->repeat & SCE_CTRL_RIGHT) && strip_focus < page_count - 1)
			strip_focus++;
		if (in->pressed & BTN_OK)
			toggle_page(strip_focus);
		else if (in->pressed & SCE_CTRL_TRIANGLE)
			open_picker(PART_WALLPAPERS);
		if (in->pressed & (BTN_BACK | SCE_CTRL_UP | SCE_CTRL_DOWN))
			strip_active = 0;
	} else {
		if ((in->repeat & SCE_CTRL_UP) && focus > 0)
			focus--;
		if ((in->repeat & SCE_CTRL_DOWN) && focus < ROW_COUNT - 1)
			focus++;
		if ((in->pressed & SCE_CTRL_RIGHT) && focus == ROW_WALLPAPERS)
			activate(ROW_WALLPAPERS);
		else if (in->pressed & BTN_OK)
			activate(focus);
		else if ((in->pressed & SCE_CTRL_TRIANGLE) && row_part(focus) != PART_COUNT)
			open_picker(row_part(focus));
		else if (in->pressed & BTN_BACK)
			close_studio();
	}

	if (in->tapped) {
		int x = in->tap_x, y = in->tap_y;
		if (y < HEADER_H && x < 200) {
			close_studio();
		} else if (y >= STRIP_Y && y <= STRIP_Y + THUMB_H && x < COL_X) {
			for (int i = 0; i < page_count; i++) {
				if (x >= strip_x(i) && x <= strip_x(i) + THUMB_W) {
					strip_active = 1;
					strip_focus = i;
					toggle_page(i);
				}
			}
		} else if (x >= COL_X && x <= COL_X + COL_W) {
			strip_active = 0;
			for (int r = 0; r < ROW_SAVE; r++) {
				if (y >= row_y(r) && y <= row_y(r) + ROW_H) {
					focus = r;
					activate(r);
				}
			}
			if (y >= SAVE_Y && y <= SAVE_Y + SAVE_H) {
				focus = ROW_SAVE;
				activate(ROW_SAVE);
			}
		}
	}

	// Keep the focused thumbnail inside the strip's visible width.
	float target = strip_scroll;
	float fx = (float)strip_focus * (THUMB_W + THUMB_GAP);
	if (fx - target < 0)
		target = fx;
	if (fx + THUMB_W - target > PREVIEW_W)
		target = fx + THUMB_W - PREVIEW_W;
	strip_scroll = ui_approach(strip_scroll, target, 16.0f);
}

// --- drawing ------------------------------------------------------------------------

static void focus_ring(float x, float y, float w, float h, float r, float a) {
	ui_rrect_outline(x - 4, y - 4, w + 8, h + 8, r + 4, 2.5f, ui_alpha(C_ACCENT, a));
}

static void draw_switch(float x, float cy, int on, float a) {
	float w = 44.0f, h = 24.0f;
	ui_rrect(x, cy - h * 0.5f, w, h, h * 0.5f, ui_alpha(on ? C_ACCENT : C_SURFACE3, a));
	float knob = on ? x + w - h * 0.5f : x + h * 0.5f;
	vita2d_draw_fill_circle(knob, cy, 9.0f, ui_alpha(C_TEXT, a));
}

// "from <title>" when a part comes from another theme.
static void origin_note(ThemePart part, char *out, size_t size) {
	int o = themedoc_part_origin(&work, part);
	if (o > 0)
		snprintf(out, size, " \xC2\xB7 from %s", work.origin_titles[o]);
	else
		out[0] = '\0';
}

static void draw_row(int row, const char *label, const char *value, int with_switch, float a) {
	float y = row_y(row);
	int focused = !strip_active && focus == row;
	ui_rrect(COL_X, y, COL_W, ROW_H, 12.0f, ui_alpha(focused ? C_SURFACE2 : C_SURFACE, a));
	ui_text(FONT_SMALL, COL_X + 14.0f, y + 19.0f, ui_alpha(C_TEXT3, a), label);
	char fitted[200];
	ui_text_fit(FONT_BOLD, value, (int)COL_W - (with_switch ? 84 : 28), fitted, sizeof(fitted));
	ui_text(FONT_BOLD, COL_X + 14.0f, y + 39.0f, ui_alpha(C_TEXT, a), fitted);
	if (focused)
		focus_ring(COL_X, y, COL_W, ROW_H, 12.0f, a);
}

static void draw_picker(float a) {
	ui_rect(0, 0, SCREEN_W, SCREEN_H, ui_alpha(C_SCRIM, a));
	float px = (SCREEN_W - PICK_W) * 0.5f, py = (SCREEN_H - PICK_H) * 0.5f;
	ui_shadow(px, py, PICK_W, PICK_H, 18.0f, a * 1.6f);
	ui_rrect(px, py, PICK_W, PICK_H, 18.0f, ui_alpha(C_SURFACE, a));
	static const char *const nouns[PART_COUNT] = {"wallpapers", "music", "icons", "lock screen", "status bar"};
	char title[96];
	snprintf(title, sizeof(title), "Take the %s from...", nouns[picker_part]);
	ui_text(FONT_TITLE, px + 24.0f, py + 40.0f, ui_alpha(C_TEXT, a), title);

	float list_y = py + 64.0f;
	vita2d_enable_clipping();
	vita2d_set_clip_rectangle((int)px, (int)list_y - 4, (int)(px + PICK_W), (int)(list_y + PICK_ROWS * PICK_ROW_H) + 4);
	for (int i = 0; i < picker_count(); i++) {
		float y = list_y + (float)i * PICK_ROW_H - picker_scroll;
		if (y + PICK_ROW_H < list_y - 4 || y > list_y + PICK_ROWS * PICK_ROW_H)
			continue;
		const LibraryItem *it = picker_item(i);
		int usable = picker_usable(i);
		float fade = usable ? 1.0f : 0.45f;
		if (i == picker_focus)
			ui_rrect(px + 12.0f, y, PICK_W - 24.0f, PICK_ROW_H - 6.0f, 12.0f, ui_alpha(C_SURFACE3, a));
		ui_rrect(px + 20.0f, y + 5.0f, 76.0f, 42.0f, 6.0f, ui_alpha(C_SURFACE2, a));
		if (!it) { // your pictures: a little mountain-and-sun photo icon
			float ix = px + 58.0f, iy = y + 26.0f;
			ui_rrect_outline(ix - 16.0f, iy - 11.0f, 32.0f, 22.0f, 4.0f, 2.0f, ui_alpha(C_TEXT2, a));
			ui_line(ix - 12.0f, iy + 8.0f, ix - 3.0f, iy - 2.0f, 2.0f, ui_alpha(C_TEXT2, a));
			ui_line(ix - 3.0f, iy - 2.0f, ix + 5.0f, iy + 8.0f, 2.0f, ui_alpha(C_TEXT2, a));
			vita2d_draw_fill_circle(ix + 7.0f, iy - 4.0f, 3.0f, ui_alpha(C_ACCENT, a));
			ui_text(FONT_BOLD, px + 110.0f, y + 23.0f, ui_alpha(C_TEXT, a), "Your pictures");
			ui_text(FONT_SMALL, px + 110.0f, y + 42.0f, ui_alpha(C_TEXT3, a),
			        picker_part == PART_WALLPAPERS ? "Add screenshots and photos as new pages"
			                                       : "Use a screenshot or photo");
			continue;
		}
		if (it->thumb[0]) {
			ImageRef img = images_file(it->thumb);
			if (img.tex)
				ui_image(img.tex, px + 20.0f, y + 5.0f, 76.0f, 42.0f, 6, 6, 6, 6,
				         RGBA8(255, 255, 255, (int)(255 * a * img.alpha * fade)));
		}
		char text[160];
		ui_text_fit(FONT_BOLD, it->title, (int)PICK_W - 160, text, sizeof(text));
		ui_text(FONT_BOLD, px + 110.0f, y + 23.0f, ui_alpha(C_TEXT, a * fade), text);
		const char *note = strcmp(it->dir, base_dir) == 0 ? "This theme" : it->provider;
		ui_text_fit(FONT_SMALL, usable ? note : "Files missing", (int)PICK_W - 160, text, sizeof(text));
		ui_text(FONT_SMALL, px + 110.0f, y + 42.0f, ui_alpha(C_TEXT3, a * fade), text);
	}
	vita2d_disable_clipping();
}

void studio_draw(void) {
	if (!is_open)
		return;
	float a = appear;
	float dy = (1.0f - a) * 20.0f;
	ui_rect(0, 0, SCREEN_W, SCREEN_H, ui_alpha(C_BG, a));
	ui_vgradient(0, 0, SCREEN_W, 160.0f, ui_alpha(C_ACCENT2, 0.10f * a), ui_alpha(C_ACCENT2, 0.0f));

	// Top bar
	ui_glyph(ui_back_glyph(), MARGIN + 10.0f, 34.0f, a);
	ui_text(FONT_BOLD, MARGIN + 28.0f, 40.0f, ui_alpha(C_TEXT2, a), "Back");
	char heading[200], fitted[200];
	snprintf(heading, sizeof(heading), "Theme Studio \xC2\xB7 %s", base_title);
	ui_text_fit(FONT_TITLE, heading, 560, fitted, sizeof(fitted));
	ui_text(FONT_TITLE, MARGIN + 110.0f, 41.0f, ui_alpha(C_TEXT, a), fitted);

	// Large preview: the lock screen when that row is focused, else a wallpaper.
	int show_lock = !strip_active && focus == ROW_LOCKSCREEN;
	int shown = strip_active ? strip_focus : first_kept();
	char path[512], caption[96];
	float py = PREVIEW_Y + dy;
	ui_rrect(PREVIEW_X, py, PREVIEW_W, PREVIEW_H, 16.0f, ui_alpha(C_SURFACE2, a));
	int have = show_lock ? (themedoc_lockscreen_file(&work, path, sizeof(path)), path[0] != '\0')
	                     : themedoc_page_file(&work, shown, 0, path, sizeof(path)) == 0;
	if (have) {
		ImageRef img = images_file(path);
		if (img.tex) {
			float dim = show_lock || page_keep[shown] ? 1.0f : 0.35f;
			int v = (int)(255 * dim);
			ui_image(img.tex, PREVIEW_X, py, PREVIEW_W, PREVIEW_H, 16, 16, 16, 16,
			         RGBA8(v, v, v, (int)(255 * a * img.alpha)));
		} else if (!img.failed) {
			ui_spinner(PREVIEW_X + PREVIEW_W * 0.5f, py + PREVIEW_H * 0.5f, 14.0f, ui_alpha(C_ACCENT, a));
		}
	}
	if (show_lock)
		snprintf(caption, sizeof(caption), "Lock screen");
	else
		snprintf(caption, sizeof(caption), "Page %d of %d%s", shown + 1, page_count,
		         page_keep[shown] ? "" : " \xC2\xB7 off");
	int cw = ui_text_width(FONT_SMALL, caption);
	ui_rrect(PREVIEW_X + 12.0f, py + 12.0f, (float)cw + 20.0f, 24.0f, 12.0f, RGBA8(0, 0, 0, (int)(150 * a)));
	ui_text(FONT_SMALL, PREVIEW_X + 22.0f, py + 28.5f, ui_alpha(C_TEXT, a), caption);

	// Wallpaper strip
	vita2d_enable_clipping();
	vita2d_set_clip_rectangle((int)PREVIEW_X - 6, (int)STRIP_Y - 6, (int)(PREVIEW_X + PREVIEW_W) + 6,
	                          (int)(STRIP_Y + THUMB_H) + 6);
	for (int i = 0; i < page_count; i++) {
		float tx = strip_x(i), ty = STRIP_Y + dy;
		if (tx + THUMB_W < PREVIEW_X - 10 || tx > PREVIEW_X + PREVIEW_W + 10)
			continue;
		ui_rrect(tx, ty, THUMB_W, THUMB_H, 8.0f, ui_alpha(C_SURFACE2, a));
		if (themedoc_page_file(&work, i, 1, path, sizeof(path)) == 0) {
			ImageRef img = images_file_sized(path, IMAGES_CARD_WIDTH);
			int v = page_keep[i] ? 255 : 80;
			if (img.tex)
				ui_image(img.tex, tx, ty, THUMB_W, THUMB_H, 8, 8, 8, 8, RGBA8(v, v, v, (int)(255 * a * img.alpha)));
		}
		float bx = tx + THUMB_W - 13.0f, by = ty + 13.0f;
		vita2d_draw_fill_circle(bx, by, 9.0f, ui_alpha(page_keep[i] ? C_ACCENT : C_SURFACE3, a));
		if (page_keep[i]) {
			ui_line(bx - 4.0f, by, bx - 1.0f, by + 3.5f, 2.0f, ui_alpha(C_ON_ACCENT, a));
			ui_line(bx - 1.0f, by + 3.5f, bx + 4.5f, by - 3.5f, 2.0f, ui_alpha(C_ON_ACCENT, a));
		}
		if (strip_active && strip_focus == i)
			focus_ring(tx, ty, THUMB_W, THUMB_H, 8.0f, a);
	}
	vita2d_disable_clipping();

	// Settings column
	char value[200], note[160];
	draw_row(ROW_NAME, "Name", name, 0, a);
	origin_note(PART_WALLPAPERS, note, sizeof(note));
	if (pictures_in_pages)
		snprintf(note, sizeof(note), " \xC2\xB7 with your pictures");
	snprintf(value, sizeof(value), "%d of %d pages%s", kept_pages(), page_count, note);
	draw_row(ROW_WALLPAPERS, "Wallpapers", value, 0, a);

	int has_music = themedoc_has_part(&work, PART_MUSIC);
	origin_note(PART_MUSIC, note, sizeof(note));
	snprintf(value, sizeof(value), "%s%s", music_on ? "On" : "Off", note);
	draw_row(ROW_MUSIC, "Background music", has_music ? value : "None (\xE2\x96\xB3 to take some)", has_music, a);
	if (has_music)
		draw_switch(COL_X + COL_W - 58.0f, row_y(ROW_MUSIC) + ROW_H * 0.5f, music_on, a);

	int icons = themedoc_icon_count(&work);
	origin_note(PART_ICONS, note, sizeof(note));
	if (icons_on)
		snprintf(value, sizeof(value), "%d custom%s", icons, note);
	else
		snprintf(value, sizeof(value), "System icons");
	draw_row(ROW_ICONS, "Icons", icons ? value : "System icons", icons > 0, a);
	if (icons)
		draw_switch(COL_X + COL_W - 58.0f, row_y(ROW_ICONS) + ROW_H * 0.5f, icons_on, a);

	origin_note(PART_LOCKSCREEN, note, sizeof(note));
	snprintf(value, sizeof(value), "%s%s", note[0] ? "Mixed in" : "Original", note);
	draw_row(ROW_LOCKSCREEN, "Lock screen", lock_from_picture ? "Your picture" : value, 0, a);
	origin_note(PART_STATUSBAR, note, sizeof(note));
	snprintf(value, sizeof(value), "%s%s", note[0] ? "Mixed in" : "Original", note);
	draw_row(ROW_STATUSBAR, "Status bar & notifications", value, 0, a);

	ui_rrect(COL_X, SAVE_Y, COL_W, SAVE_H, 14.0f, ui_alpha(C_ACCENT, a));
	ui_text_center(FONT_TITLE, COL_X + COL_W * 0.5f, SAVE_Y + 30.0f, ui_alpha(C_ON_ACCENT, a), "Save as new theme");
	if (!strip_active && focus == ROW_SAVE)
		focus_ring(COL_X, SAVE_Y, COL_W, SAVE_H, 14.0f, a);
	ui_text_center(FONT_SMALL, COL_X + COL_W * 0.5f, SAVE_Y + SAVE_H + 20.0f, ui_alpha(C_TEXT3, a),
	               "The original theme stays as it is");

	if (picture_picker_is_open()) {
		picture_picker_draw();
	} else if (picking) {
		draw_picker(a);
		Hint hints[] = {{ui_back_glyph(), "Cancel"}, {ui_ok_glyph(), "Take"}};
		app_footer(hints, 2, NULL);
	} else if (strip_active) {
		Hint hints[] = {{ui_back_glyph(), "Done"}, {GLYPH_TRIANGLE, "Take from..."}, {ui_ok_glyph(), "Keep / remove"}};
		app_footer(hints, 3, NULL);
	} else {
		Hint hints[3];
		int n = 0;
		hints[n++] = (Hint){ui_back_glyph(), "Back"};
		if (row_part(focus) != PART_COUNT)
			hints[n++] = (Hint){GLYPH_TRIANGLE, "Take from..."};
		hints[n++] = (Hint){ui_ok_glyph(), "Select"};
		app_footer(hints, n, NULL);
	}
}
