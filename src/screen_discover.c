// Discover: the repository as an endless grid of theme cards.
#include "app.h"
#include "config.h"
#include "dialog.h"
#include "images.h"
#include "ime.h"
#include "jobs.h"
#include "tasks.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLS 4
#define GAP 16.0f
#define CARD_W ((SCREEN_W - 2 * MARGIN - (COLS - 1) * GAP) / COLS)
#define THUMB_H ((float)(int)(CARD_W * 544.0f / 960.0f))
#define CARD_H (THUMB_H + 58.0f)
#define PITCH (CARD_H + 18.0f)
#define CHIP_Y (HEADER_H + 6.0f)
#define CHIP_H 32.0f
#define GRID_TOP (HEADER_H + 56.0f)
#define GRID_BOTTOM (FOOTER_Y - 8.0f)
#define VIEW_H (GRID_BOTTOM - GRID_TOP)
#define THUMB_WIDTH 216 // requested preview width (≈8 KB JPEG)
#define PREFETCH_ITEMS (COLS * 3)

static PsvtQuery query;
static PsvtTheme *items;
static int count, capacity;
static int pages_loaded, total_pages = 1;
static int loading, failed;
static char error_text[160];
static int generation;
static int selected;
static float scroll, scroll_target, stick_accum;
static uint32_t new_threshold; // themes newer than the last session get a NEW badge

// Browsing history. Applying a filter saves the grid as it was (loaded themes,
// scroll position and highlighted card) so Back returns to exactly the same
// place without downloading anything again.
#define MAX_HISTORY 8
typedef struct {
	PsvtQuery query;
	PsvtTheme *items;
	int count, capacity, pages_loaded, total_pages, selected;
	float scroll;
} GridState;
static GridState history[MAX_HISTORY];
static int history_count;

// Chip hit boxes, recorded while drawing for touch input.
static float chip_back_x, chip_back_w, chip_sort_x, chip_sort_w, chip_search_x, chip_search_w, chip_filter_x,
    chip_filter_w;

typedef struct {
	int generation;
	int page;
	PsvtQuery query;
	PsvtPage result;
	int rc;
	char error[160];
} PageJob;

static void page_work(void *ctx, NetClient *net) {
	PageJob *job = ctx;
	char url[512];
	psvt_list_url(url, sizeof(url), &job->query, job->page);
	char *html = NULL;
	if (!net_online()) {
		job->rc = -1;
		snprintf(job->error, sizeof(job->error), "You're offline. Connect to Wi-Fi and try again.");
		return;
	}
	if (net_get(net, url, &html, NULL, 2 * 1024 * 1024, NULL, NULL) != NET_OK) {
		job->rc = -1;
		snprintf(job->error, sizeof(job->error), "Couldn't reach psvt.ovh (%s).", net_error(net));
		return;
	}
	if (psvt_parse_list(html, &job->result) < 0) {
		job->rc = -1;
		snprintf(job->error, sizeof(job->error), "psvt.ovh sent a page Imago doesn't understand.");
	}
	free(html);
}

static void append(const PsvtTheme *t) {
	for (int i = 0; i < count; i++)
		if (items[i].id == t->id)
			return; // pages shift when themes are added mid-scroll
	if (count == capacity) {
		int cap = capacity ? capacity * 2 : 128;
		PsvtTheme *grown = realloc(items, (size_t)cap * sizeof(PsvtTheme));
		if (!grown)
			return;
		items = grown;
		capacity = cap;
	}
	items[count++] = *t;
}

static void page_done(void *ctx) {
	PageJob *job = ctx;
	if (job->generation == generation) {
		loading = 0;
		if (job->rc == 0) {
			failed = 0;
			pages_loaded = job->page;
			total_pages = job->result.total_pages > 0 ? job->result.total_pages : 1;
			uint32_t newest = g_config.last_seen_id;
			for (int i = 0; i < job->result.count; i++) {
				append(&job->result.items[i]);
				if (job->result.items[i].id > newest)
					newest = job->result.items[i].id;
			}
			if (newest != g_config.last_seen_id) {
				g_config.last_seen_id = newest;
				config_save();
			}
		} else {
			failed = 1;
			snprintf(error_text, sizeof(error_text), "%s", job->error);
		}
	}
	free(job);
}

static void request_page(int page) {
	PageJob *job = calloc(1, sizeof(*job));
	if (!job)
		return;
	job->generation = generation;
	job->page = page;
	job->query = query;
	loading = 1;
	failed = 0;
	jobs_submit(page_work, page_done, job);
}

static void reload(void) {
	generation++;
	count = 0;
	pages_loaded = 0;
	total_pages = 1;
	selected = 0;
	scroll = scroll_target = 0.0f;
	request_page(1);
}

void discover_init(void) {
	query.sort = (PsvtSort)g_config.sort;
	new_threshold = g_config.last_seen_id;
	reload();
}

int discover_ready(void) {
	return pages_loaded > 0 || failed;
}

static int has_filter(void) {
	return query.search[0] || query.tag[0] || query.author[0];
}

// Moves the current grid onto the history stack; the grid starts empty.
static void push_history(void) {
	if (count == 0)
		return; // nothing worth returning to
	if (history_count == MAX_HISTORY) {
		free(history[0].items);
		memmove(history, history + 1, sizeof(history[0]) * (MAX_HISTORY - 1));
		history_count--;
	}
	history[history_count++] = (GridState){query, items, count, capacity, pages_loaded, total_pages, selected,
	                                       scroll_target};
	items = NULL;
	count = capacity = 0;
}

static void restore(const GridState *state) {
	free(items);
	query = state->query;
	items = state->items;
	count = state->count;
	capacity = state->capacity;
	pages_loaded = state->pages_loaded;
	total_pages = state->total_pages;
	selected = state->selected;
	scroll = scroll_target = state->scroll;
	generation++; // ignore pages still loading for the grid we are leaving
	loading = failed = 0;
	if (g_config.sort != (int)query.sort) {
		g_config.sort = query.sort;
		config_save();
	}
}

static void go_back(void) {
	if (history_count > 0)
		restore(&history[--history_count]);
}

// SELECT: back to where browsing started, then drop any remaining filter.
static void go_home(void) {
	if (history_count > 0) {
		for (int i = 1; i < history_count; i++)
			free(history[i].items);
		restore(&history[0]);
		history_count = 0;
	}
	if (has_filter()) {
		query.search[0] = query.tag[0] = query.author[0] = '\0';
		reload();
	}
}

static void apply_filter(const char *search, const char *tag, const char *author) {
	if (strcmp(query.search, search) == 0 && strcmp(query.tag, tag) == 0 && strcmp(query.author, author) == 0)
		return; // already showing exactly this
	push_history();
	snprintf(query.search, sizeof(query.search), "%s", search);
	snprintf(query.tag, sizeof(query.tag), "%s", tag);
	snprintf(query.author, sizeof(query.author), "%s", author);
	reload();
}

void discover_set_filter(const char *tag, const char *author) {
	apply_filter("", tag ? tag : "", author ? author : "");
}

const PsvtTheme *discover_find(uint32_t id) {
	for (int i = 0; i < count; i++)
		if (items[i].id == id)
			return &items[i];
	return NULL;
}

static void on_sort(int choice) {
	if (choice < 0 || choice == (int)query.sort)
		return;
	query.sort = (PsvtSort)choice;
	g_config.sort = choice;
	config_save();
	reload();
}

static void open_sort_menu(void) {
	const char *names[PSVT_SORT_COUNT];
	for (int i = 0; i < PSVT_SORT_COUNT; i++)
		names[i] = psvt_sort_name((PsvtSort)i);
	dialog_menu("Sort themes by", names, PSVT_SORT_COUNT, (int)query.sort, on_sort);
}

static void open_search(void) {
	ime_open("Search themes", query.search, 60);
}

static float content_height(void) {
	int rows = (count + COLS - 1) / COLS;
	return rows > 0 ? (float)rows * PITCH - 18.0f + 40.0f : 0.0f;
}

static float max_scroll(void) {
	float m = content_height() - VIEW_H;
	return m > 0 ? m : 0;
}

static void keep_selection_visible(void) {
	float top = (float)(selected / COLS) * PITCH;
	if (top - 12.0f < scroll_target)
		scroll_target = top - 12.0f;
	if (top + CARD_H + 16.0f > scroll_target + VIEW_H)
		scroll_target = top + CARD_H + 16.0f - VIEW_H;
	if (scroll_target < 0)
		scroll_target = 0;
}

static void move(int delta) {
	if (count == 0)
		return;
	int next = selected + delta;
	if (delta == 1 && selected % COLS == COLS - 1)
		return;
	if (delta == -1 && selected % COLS == 0)
		return;
	if (next < 0)
		next = selected % COLS == selected ? selected : 0;
	if (next >= count) {
		if (failed)
			failed = 0; // pushing past the end retries a failed "load more"
		next = delta == COLS && selected / COLS < (count - 1) / COLS ? count - 1 : selected;
	}
	selected = next;
	keep_selection_visible();
}

static int card_at(int tx, int ty) {
	if (ty < GRID_TOP || ty > GRID_BOTTOM)
		return -1;
	float cy = (float)ty - GRID_TOP + scroll;
	int row = (int)(cy / PITCH);
	int col = (int)(((float)tx - MARGIN) / (CARD_W + GAP));
	if (col < 0 || col >= COLS || cy - (float)row * PITCH > CARD_H)
		return -1;
	float cx = (float)tx - MARGIN - (float)col * (CARD_W + GAP);
	if (cx > CARD_W)
		return -1;
	int index = row * COLS + col;
	return index < count ? index : -1;
}

void discover_update(const Input *in) {
	if (ime_active()) {
		char text[128];
		int r = ime_poll(text, sizeof(text));
		if (r == 1)
			apply_filter(text, "", "");
		return;
	}

	if (in->repeat & SCE_CTRL_LEFT)
		move(-1);
	if (in->repeat & SCE_CTRL_RIGHT)
		move(1);
	if (in->repeat & SCE_CTRL_UP)
		move(-COLS);
	if (in->repeat & SCE_CTRL_DOWN)
		move(COLS);
	if (in->ly != 0.0f) {
		stick_accum += in->ly * ui_dt() * 9.0f;
		while (stick_accum >= 1.0f) {
			move(COLS);
			stick_accum -= 1.0f;
		}
		while (stick_accum <= -1.0f) {
			move(-COLS);
			stick_accum += 1.0f;
		}
	} else {
		stick_accum = 0.0f;
	}

	if (in->pressed & BTN_OK) {
		if (count > 0)
			detail_open(&items[selected]);
		else if (failed)
			request_page(1);
	} else if (in->pressed & SCE_CTRL_TRIANGLE) {
		open_sort_menu();
	} else if (in->pressed & SCE_CTRL_SQUARE) {
		open_search();
	} else if (in->pressed & SCE_CTRL_SELECT) {
		go_home();
	} else if (in->pressed & BTN_BACK) {
		go_back();
	}

	if (in->dragging) {
		scroll_target -= in->drag_dy;
		scroll = scroll_target;
		// Keep the highlighted card on screen while the user drags.
		int first_row = (int)ceilf(scroll_target / PITCH);
		int last_row = (int)floorf((scroll_target + VIEW_H - CARD_H) / PITCH);
		int row = selected / COLS;
		if (row < first_row)
			selected = first_row * COLS + selected % COLS;
		else if (row > last_row && last_row >= 0)
			selected = last_row * COLS + selected % COLS;
		if (selected >= count)
			selected = count - 1;
		if (selected < 0)
			selected = 0;
	}
	if (in->tapped) {
		int ty = in->tap_y, tx = in->tap_x;
		if (ty >= CHIP_Y && ty <= CHIP_Y + CHIP_H) {
			if (chip_back_w > 0 && tx >= chip_back_x && tx <= chip_back_x + chip_back_w)
				go_back();
			else if (tx >= chip_sort_x && tx <= chip_sort_x + chip_sort_w)
				open_sort_menu();
			else if (tx >= chip_search_x && tx <= chip_search_x + chip_search_w)
				open_search();
			else if (chip_filter_w > 0 && tx >= chip_filter_x && tx <= chip_filter_x + chip_filter_w)
				go_home();
		} else {
			int hit = card_at(tx, ty);
			if (hit >= 0) {
				selected = hit;
				detail_open(&items[hit]);
			} else if (failed) {
				request_page(pages_loaded + 1);
			}
		}
	}

	float limit = max_scroll();
	if (scroll_target > limit)
		scroll_target = limit;
	if (scroll_target < 0)
		scroll_target = 0;
	scroll = ui_approach(scroll, scroll_target, 16.0f);

	int near_end = selected >= count - PREFETCH_ITEMS || scroll_target + VIEW_H > content_height() - PITCH * 2;
	if (!loading && !failed && pages_loaded < total_pages && near_end)
		request_page(pages_loaded + 1);
}

// --- drawing ------------------------------------------------------------------

static float draw_chip(float x, Glyph glyph, const char *label, int accent) {
	char fitted[96];
	ui_text_fit(FONT_BOLD, label, 220, fitted, sizeof(fitted));
	float gw = (float)ui_glyph_width(glyph);
	float w = 12.0f + gw + 9.0f + (float)ui_text_width(FONT_BOLD, fitted) + 16.0f;
	ui_rrect(x, CHIP_Y, w, CHIP_H, CHIP_H * 0.5f, accent ? ui_alpha(C_ACCENT, 0.22f) : C_SURFACE);
	ui_glyph(glyph, x + 12.0f + gw * 0.5f, CHIP_Y + CHIP_H * 0.5f, 1.0f);
	ui_text(FONT_BOLD, x + 12.0f + gw + 9.0f, CHIP_Y + 21.5f, accent ? C_TEXT : C_TEXT2, fitted);
	return w;
}

static void draw_card(int index, float x, float y) {
	const PsvtTheme *t = &items[index];
	int focused = index == selected;
	if (focused)
		ui_shadow(x, y, CARD_W, CARD_H, 14.0f, 1.0f);
	ui_rrect(x, y, CARD_W, CARD_H, 14.0f, focused ? C_SURFACE2 : C_SURFACE);

	ImageRef img = images_remote(t->hash, THUMB_WIDTH);
	if (!img.tex || img.alpha < 1.0f) {
		float pulse = 0.5f + 0.5f * sinf(ui_time() * 3.0f + (float)index * 0.4f);
		uint32_t ph = ui_mix(C_SURFACE2, C_SURFACE3, img.failed ? 0.0f : pulse * 0.6f);
		ui_rrect(x, y, CARD_W, THUMB_H, 14.0f, ph);
		ui_rect(x, y + THUMB_H - 14.0f, CARD_W, 14.0f, ph);
	}
	if (img.tex)
		ui_image(img.tex, x, y, CARD_W, THUMB_H, 14, 14, 0, 0, RGBA8(255, 255, 255, (int)(255 * img.alpha)));

	char text[160];
	ui_text_fit(FONT_BOLD, t->title, (int)CARD_W - 24, text, sizeof(text));
	ui_text(FONT_BOLD, x + 12.0f, y + THUMB_H + 25.0f, C_TEXT, text);

	// Status badge (right side of the meta line)
	Task task;
	const char *badge = NULL;
	uint32_t bg = 0, fg = 0;
	int running = tasks_find(t->id, &task) && task.state <= TASK_FINISHING;
	if (running) {
		ui_rect(x, y + THUMB_H - 30.0f, CARD_W, 30.0f, RGBA8(0, 0, 0, 120));
		ui_progress(x + 12.0f, y + THUMB_H - 17.0f, CARD_W - 24.0f, 6.0f, task.progress,
		            RGBA8(255, 255, 255, 60), C_ACCENT);
		badge = task.state == TASK_QUEUED ? "QUEUED" : "INSTALLING";
		bg = ui_alpha(C_ACCENT, 0.2f);
		fg = C_ACCENT;
	} else {
		ThemeStatus status = library_status(&g_library, t->id, t->hash, t->title);
		if (status == THEME_UPDATE) {
			badge = "UPDATE";
			bg = ui_alpha(C_WARNING, 0.18f);
			fg = C_WARNING;
		} else if (status == THEME_INSTALLED) {
			badge = "INSTALLED";
			bg = ui_alpha(C_SUCCESS, 0.16f);
			fg = C_SUCCESS;
		} else if (new_threshold && t->id > new_threshold) {
			badge = "NEW";
			bg = ui_alpha(C_ACCENT, 0.22f);
			fg = C_ACCENT;
		}
	}
	float meta_right = x + CARD_W - 12.0f;
	if (badge) {
		float bw = (float)ui_badge_width(badge);
		ui_badge(meta_right - bw, y + THUMB_H + 31.0f, badge, bg, fg);
		meta_right -= bw + 8.0f;
	}
	if (t->downloads >= 0) {
		// Download icon (an arrow onto a tray), then the count, kept clear of the badge.
		float ix = x + 12.0f, iy = y + THUMB_H + 34.0f;
		ui_line(ix + 5.0f, iy, ix + 5.0f, iy + 7.0f, 1.6f, C_TEXT3);
		ui_line(ix + 1.8f, iy + 4.0f, ix + 5.0f, iy + 7.4f, 1.6f, C_TEXT3);
		ui_line(ix + 8.2f, iy + 4.0f, ix + 5.0f, iy + 7.4f, 1.6f, C_TEXT3);
		ui_line(ix, iy + 10.0f, ix + 10.0f, iy + 10.0f, 1.6f, C_TEXT3);
		char downloads[32];
		app_format_count(downloads, sizeof(downloads), t->downloads);
		ui_text_fit(FONT_SMALL, downloads, (int)(meta_right - ix - 16.0f), text, sizeof(text));
		ui_text(FONT_SMALL, ix + 16.0f, y + THUMB_H + 45.0f, C_TEXT3, text);
	}

	if (focused) {
		float pulse = 0.75f + 0.25f * sinf(ui_time() * 4.0f);
		ui_rrect_outline(x - 4.0f, y - 4.0f, CARD_W + 8.0f, CARD_H + 8.0f, 18.0f, 2.5f, ui_alpha(C_ACCENT, pulse));
	}
}

static void draw_skeleton(void) {
	float pulse = 0.5f + 0.5f * sinf(ui_time() * 3.0f);
	for (int i = 0; i < COLS * 2; i++) {
		float x = MARGIN + (float)(i % COLS) * (CARD_W + GAP);
		float y = GRID_TOP + (float)(i / COLS) * PITCH;
		ui_rrect(x, y, CARD_W, CARD_H, 14.0f, C_SURFACE);
		ui_rrect(x, y, CARD_W, THUMB_H, 14.0f, ui_mix(C_SURFACE2, C_SURFACE3, pulse * 0.6f));
		ui_rect(x, y + THUMB_H - 14.0f, CARD_W, 14.0f, ui_mix(C_SURFACE2, C_SURFACE3, pulse * 0.6f));
		ui_rrect(x + 12.0f, y + THUMB_H + 13.0f, CARD_W * 0.6f, 12.0f, 6.0f, C_SURFACE2);
		ui_rrect(x + 12.0f, y + THUMB_H + 35.0f, CARD_W * 0.35f, 10.0f, 5.0f, C_SURFACE2);
	}
}

static void draw_message(const char *title, const char *body, int retry) {
	float cy = GRID_TOP + VIEW_H * 0.42f;
	ui_text_center(FONT_TITLE, SCREEN_W * 0.5f, cy, C_TEXT, title);
	ui_text_center(FONT_BODY, SCREEN_W * 0.5f, cy + 30.0f, C_TEXT2, body);
	if (retry) {
		float w = 150.0f;
		ui_rrect(SCREEN_W * 0.5f - w * 0.5f, cy + 52.0f, w, 40.0f, 12.0f, C_ACCENT);
		ui_glyph(ui_ok_glyph(), SCREEN_W * 0.5f - 34.0f, cy + 72.0f, 1.0f);
		ui_text(FONT_BOLD, SCREEN_W * 0.5f - 18.0f, cy + 78.0f, C_ON_ACCENT, "Retry");
	}
}

void discover_draw(void) {
	// Filter chips
	char label[128];
	float x = MARGIN;
	chip_back_w = 0;
	if (history_count > 0) {
		chip_back_x = x;
		chip_back_w = draw_chip(x, ui_back_glyph(), "Back", 0);
		x += chip_back_w + 10.0f;
	}
	chip_sort_x = x;
	chip_sort_w = draw_chip(x, GLYPH_TRIANGLE, psvt_sort_name(query.sort), 0);
	x += chip_sort_w + 10.0f;
	if (query.search[0])
		snprintf(label, sizeof(label), "\xE2\x80\x9C%s\xE2\x80\x9D", query.search);
	else
		snprintf(label, sizeof(label), "Search");
	chip_search_x = x;
	chip_search_w = draw_chip(x, GLYPH_SQUARE, label, query.search[0] != 0);
	x += chip_search_w + 10.0f;
	chip_filter_w = 0;
	if (query.tag[0] || query.author[0]) {
		if (query.tag[0])
			snprintf(label, sizeof(label), "#%s", query.tag);
		else
			snprintf(label, sizeof(label), "by %s", query.author);
		chip_filter_x = x;
		chip_filter_w = draw_chip(x, GLYPH_SELECT, label, 1);
	}
	if (count > 0) {
		char status[48], n[24];
		if (pages_loaded >= total_pages) {
			app_format_count(n, sizeof(n), count);
			snprintf(status, sizeof(status), "%s theme%s", n, count == 1 ? "" : "s");
		} else {
			app_format_count(n, sizeof(n), (total_pages - 1) * (count / (pages_loaded ? pages_loaded : 1)));
			snprintf(status, sizeof(status), "%s+ themes", n);
		}
		ui_text_right(FONT_SMALL, SCREEN_W - MARGIN, CHIP_Y + 21.0f, C_TEXT3, status);
	}

	if (count == 0) {
		if (failed)
			draw_message("Something went wrong", error_text, 1);
		else if (loading)
			draw_skeleton();
		else
			draw_message("No themes found", has_filter() ? "Try another search, or press SELECT to clear filters."
			                                             : "The repository returned no themes.", 0);
	} else {
		vita2d_enable_clipping();
		vita2d_set_clip_rectangle(0, (int)GRID_TOP - 12, SCREEN_W, (int)GRID_BOTTOM + 6);
		int first = (int)floorf((scroll - 20.0f) / PITCH);
		int last = (int)ceilf((scroll + VIEW_H) / PITCH);
		if (first < 0)
			first = 0;
		for (int row = first; row <= last; row++) {
			for (int col = 0; col < COLS; col++) {
				int i = row * COLS + col;
				if (i >= count)
					break;
				float cx = MARGIN + (float)col * (CARD_W + GAP);
				float cy = GRID_TOP + (float)row * PITCH - scroll;
				draw_card(i, cx, cy);
			}
		}
		float tail = GRID_TOP + content_height() - 40.0f - scroll + 10.0f;
		if (loading && tail < GRID_BOTTOM)
			ui_spinner(SCREEN_W * 0.5f, tail + 16.0f, 10.0f, C_ACCENT);
		else if (failed && tail < GRID_BOTTOM)
			ui_text_center(FONT_SMALL, SCREEN_W * 0.5f, tail + 20.0f, C_TEXT3,
			               "Couldn't load more themes. Tap to retry.");
		vita2d_disable_clipping();
		// Soft fade where the grid scrolls under the chips.
		if (scroll > 1.0f)
			ui_vgradient(0, GRID_TOP - 12.0f, SCREEN_W, 18.0f, C_BG, ui_alpha(C_BG, 0.0f));
	}

	Hint hints[5];
	int n = 0;
	if (has_filter() || history_count > 0)
		hints[n++] = (Hint){GLYPH_SELECT, "Clear"};
	hints[n++] = (Hint){GLYPH_SQUARE, "Search"};
	hints[n++] = (Hint){GLYPH_TRIANGLE, "Sort"};
	if (history_count > 0)
		hints[n++] = (Hint){ui_back_glyph(), "Back"};
	hints[n++] = (Hint){ui_ok_glyph(), "Details"};
	app_footer(hints, n, NULL);
}
