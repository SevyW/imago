// Searching artwork sites for one of an app's parts. A picked picture is
// downloaded, then framed in the editor like any picture from the memory card.
#include "app.h"
#include "artwork.h"
#include "dialog.h"
#include "fs.h"
#include "ime.h"
#include "images.h"
#include "log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ONLINE_DIR DATA_DIR "/online"
#define TABS_Y 76.0f
#define TAB_H 34.0f
#define GRID_Y 126.0f
#define GRID_BOTTOM 496.0f
#define GAP 12.0f
#define LABEL_H 40.0f
#define MAX_TABS 16

typedef struct {
	int source;          // artwork_get index
	ArtResults *results; // NULL until searched
	int searching;
	int generation; // results belong to this search text
	char err[ART_ERROR_SIZE];
} SourceTab;

static int is_open, closing;
static float appear, scroll, scroll_target;
static char app_id[16], title_id[16], query[128];
static ArtPart part;
static SourceTab tabs[MAX_TABS];
static int tab_count, current, selected, generation;
static int editing;

static int columns(void) {
	return part == ART_ICON ? 5 : 3;
}

static float card_w(void) {
	int cols = columns();
	return (SCREEN_W - 2 * MARGIN - (float)(cols - 1) * GAP) / (float)cols;
}

static float image_h(void) {
	return part == ART_ICON ? card_w() : card_w() * (part == ART_BG ? 500.0f / 840.0f : 158.0f / 280.0f);
}

// --- searching -------------------------------------------------------------------

typedef struct {
	int tab, generation, rc;
	ArtQuery q;
	ArtResults *results;
	char err[ART_ERROR_SIZE];
} SearchJob;

static void search_work(void *ctx, NetClient *net) {
	SearchJob *j = ctx;
	j->results = calloc(1, sizeof(ArtResults));
	if (!j->results) {
		j->rc = -1;
		snprintf(j->err, sizeof(j->err), "Not enough memory.");
		return;
	}
	const ArtSource *s = artwork_get(tabs[j->tab].source);
	j->rc = s->search(net, &j->q, j->results, j->err, sizeof(j->err));
	log_write("online: %s \"%s\" (%s): %d result%s%s%s", s->id, j->q.title, j->q.title_id, j->results->count,
	          j->results->count == 1 ? "" : "s", j->rc ? ", " : "", j->rc ? j->err : "");
}

static void search_done(void *ctx) {
	SearchJob *j = ctx;
	SourceTab *t = &tabs[j->tab];
	if (is_open && j->generation == generation && t->searching) {
		free(t->results);
		t->results = j->results;
		j->results = NULL;
		t->searching = 0;
		t->generation = j->generation;
		snprintf(t->err, sizeof(t->err), "%s", j->rc ? j->err : "");
	}
	free(j->results);
	free(j);
}

static void search(int tab) {
	SourceTab *t = &tabs[tab];
	if (t->searching || (t->results && t->generation == generation))
		return;
	SearchJob *j = calloc(1, sizeof(*j));
	if (!j)
		return;
	j->tab = tab;
	j->generation = generation;
	snprintf(j->q.title, sizeof(j->q.title), "%s", query);
	snprintf(j->q.title_id, sizeof(j->q.title_id), "%s", title_id);
	j->q.part = part;
	t->searching = 1;
	t->err[0] = '\0';
	jobs_submit(search_work, search_done, j);
}

static void clear_results(void) {
	for (int i = 0; i < tab_count; i++) {
		free(tabs[i].results);
		tabs[i].results = NULL;
		tabs[i].searching = 0;
		tabs[i].err[0] = '\0';
	}
}

void online_open(const char *id, const char *app_title, ArtPart which) {
	snprintf(app_id, sizeof(app_id), "%s", id);
	snprintf(title_id, sizeof(title_id), "%s", id);
	snprintf(query, sizeof(query), "%s", app_title);
	part = which;
	clear_results();
	tab_count = 0;
	for (int i = 0; i < artwork_count() && tab_count < MAX_TABS; i++)
		if (artwork_ready(artwork_get(i)) && (artwork_get(i)->parts & (1u << which))) {
			memset(&tabs[tab_count], 0, sizeof(tabs[0]));
			tabs[tab_count++].source = i;
		}
	current = selected = 0;
	scroll = scroll_target = 0.0f;
	generation++;
	is_open = 1;
	closing = 0;
	appear = 0.0f;
	if (tab_count)
		search(0);
}

int online_is_open(void) {
	return is_open;
}

// --- picking ---------------------------------------------------------------------

typedef struct {
	char url[512], path[256], app[16];
	ArtPart part;
	int rc;
	char err[160];
} PickJob;

static void pick_work(void *ctx, NetClient *net) {
	PickJob *j = ctx;
	fs_mkdir_p(ONLINE_DIR);
	j->rc = net_download(net, j->url, j->path, NULL, NULL);
	if (j->rc != NET_OK)
		snprintf(j->err, sizeof(j->err), "%s", net_error(net));
}

static void pick_done(void *ctx) {
	PickJob *j = ctx;
	if (j->rc != NET_OK) {
		log_write("online: download failed: %s", j->err);
		dialog_message("Couldn't download the picture", j->err);
	} else {
		closing = 1;
		editor_open(j->app, j->part, j->path);
	}
	free(j);
}

static void pick(const ArtResult *r) {
	PickJob *j = calloc(1, sizeof(*j));
	if (!j)
		return;
	snprintf(j->url, sizeof(j->url), "%s", r->full);
	snprintf(j->app, sizeof(j->app), "%s", app_id);
	j->part = part;
	// One file per part: the editor reads it, and the next pick replaces it.
	static const char *const names[ART_COUNT] = {"icon", "background", "gate"};
	snprintf(j->path, sizeof(j->path), ONLINE_DIR "/%s.img", names[part]);
	images_forget(j->path);
	app_run("Downloading the picture...", pick_work, pick_done, j);
}

// --- input -----------------------------------------------------------------------

static const ArtResults *shown(void) {
	if (!tab_count)
		return NULL;
	SourceTab *t = &tabs[current];
	return t->generation == generation ? t->results : NULL;
}

static void switch_tab(int to) {
	if (to < 0 || to >= tab_count || to == current)
		return;
	current = to;
	selected = 0;
	scroll = scroll_target = 0.0f;
	search(current);
}

static float tab_x(int i) {
	float x = MARGIN;
	for (int k = 0; k < i; k++)
		x += (float)ui_text_width(FONT_SMALL, artwork_get(tabs[k].source)->name) + 32.0f + 8.0f;
	return x;
}

void online_update(const Input *in) {
	if (closing) {
		appear = ui_approach(appear, 0.0f, 20.0f);
		if (appear < 0.02f) {
			is_open = 0;
			clear_results();
		}
		return;
	}
	appear = ui_approach(appear, 1.0f, 16.0f);
	if (editing) {
		if (ime_active()) {
			char text[128];
			int r = ime_poll(text, sizeof(text));
			if (r != 0)
				editing = 0;
			if (r == 1 && text[0] && strcmp(text, query) != 0) {
				snprintf(query, sizeof(query), "%s", text);
				generation++;
				clear_results();
				selected = 0;
				scroll = scroll_target = 0.0f;
				search(current);
			}
		} else {
			editing = 0;
		}
		return;
	}
	if (in->pressed & BTN_BACK) {
		closing = 1;
		return;
	}
	if (in->pressed & SCE_CTRL_LTRIGGER)
		switch_tab(current - 1);
	if (in->pressed & SCE_CTRL_RTRIGGER)
		switch_tab(current + 1);
	if (in->pressed & SCE_CTRL_TRIANGLE) {
		editing = 1;
		ime_open("Search for", query, (int)sizeof(query) - 1);
	}
	const ArtResults *r = shown();
	int count = r ? r->count : 0, cols = columns();
	if (count) {
		if ((in->repeat & SCE_CTRL_LEFT) && selected % cols > 0)
			selected--;
		if ((in->repeat & SCE_CTRL_RIGHT) && selected % cols < cols - 1 && selected < count - 1)
			selected++;
		if ((in->repeat & SCE_CTRL_UP) && selected >= cols)
			selected -= cols;
		if ((in->repeat & SCE_CTRL_DOWN) && selected + cols < count)
			selected += cols;
		else if ((in->repeat & SCE_CTRL_DOWN) && selected / cols < (count - 1) / cols)
			selected = count - 1;
		if (in->pressed & BTN_OK)
			pick(&r->items[selected]);
	}
	if (in->tapped) {
		if (in->tap_y < HEADER_H && in->tap_x < 200) {
			closing = 1;
		} else if (in->tap_y >= TABS_Y && in->tap_y < TABS_Y + TAB_H) {
			for (int i = tab_count - 1; i >= 0; i--)
				if (in->tap_x >= tab_x(i)) {
					switch_tab(i);
					break;
				}
		} else if (in->tap_y < HEADER_H && in->tap_x > SCREEN_W - 420) {
			editing = 1;
			ime_open("Search for", query, (int)sizeof(query) - 1);
		} else if (count && in->tap_y >= GRID_Y && in->tap_y < GRID_BOTTOM) {
			float cw = card_w(), ch = image_h() + LABEL_H;
			int col = (int)((in->tap_x - MARGIN) / (cw + GAP));
			int row = (int)((in->tap_y - GRID_Y + scroll) / (ch + GAP));
			int i = row * cols + col;
			if (col >= 0 && col < cols && i >= 0 && i < count) {
				selected = i;
				pick(&r->items[i]);
			}
		}
	}
	// Keep the selection in view.
	float ch = image_h() + LABEL_H, top = (float)(selected / cols) * (ch + GAP);
	if (top < scroll_target)
		scroll_target = top;
	if (top + ch > scroll_target + (GRID_BOTTOM - GRID_Y))
		scroll_target = top + ch - (GRID_BOTTOM - GRID_Y);
	scroll = ui_approach(scroll, scroll_target, 14.0f);
}

// --- drawing ---------------------------------------------------------------------

static void draw_message(float a, const char *title, const char *text) {
	float cy = (GRID_Y + GRID_BOTTOM) * 0.5f;
	ui_text_center(FONT_BOLD, SCREEN_W * 0.5f, cy - 8.0f, ui_alpha(C_TEXT, a), title);
	int max_w = (int)(SCREEN_W - 2 * MARGIN - 240.0f);
	if (text && ui_text_width(FONT_SMALL, text) <= max_w)
		ui_text_center(FONT_SMALL, SCREEN_W * 0.5f, cy + 22.0f, ui_alpha(C_TEXT3, a), text);
	else if (text)
		ui_text_wrap(FONT_SMALL, MARGIN + 120.0f, cy + 22.0f, max_w, 22, 4, ui_alpha(C_TEXT3, a), text);
}

void online_draw(void) {
	if (!is_open)
		return;
	float a = appear, dy = (1.0f - a) * 20.0f;
	ui_rect(0, 0, SCREEN_W, SCREEN_H, ui_alpha(C_BG, a));
	ui_vgradient(0, 0, SCREEN_W, 160.0f, ui_alpha(C_ACCENT2, 0.10f * a), ui_alpha(C_ACCENT2, 0.0f));
	ui_glyph(ui_back_glyph(), MARGIN + 10.0f, 34.0f, a);
	ui_text(FONT_BOLD, MARGIN + 28.0f, 40.0f, ui_alpha(C_TEXT2, a), "Back");

	// The search text, top right: tap it (or press triangle) to change it.
	char shown_query[160], fitted[160];
	snprintf(shown_query, sizeof(shown_query), "\"%s\"", query);
	ui_text_fit(FONT_BOLD, shown_query, 360, fitted, sizeof(fitted));
	float qw = (float)ui_text_width(FONT_BOLD, fitted);
	ui_rrect(SCREEN_W - MARGIN - qw - 56.0f, 20.0f, qw + 56.0f, 36.0f, 18.0f, ui_alpha(C_SURFACE2, a));
	ui_glyph(GLYPH_TRIANGLE, SCREEN_W - MARGIN - qw - 34.0f, 38.0f, a);
	ui_text(FONT_BOLD, SCREEN_W - MARGIN - qw - 18.0f, 44.0f, ui_alpha(C_TEXT, a), fitted);

	if (!tab_count) {
		draw_message(a, "No artwork source is set up",
		             "Add a key in Settings \xE2\x80\xBA Artwork sources (SteamGridDB, IGDB and others are free).");
		Hint hints[1] = {{ui_back_glyph(), "Back"}};
		app_footer(hints, 1, NULL);
		return;
	}

	// Source tabs
	for (int i = 0; i < tab_count; i++) {
		const char *name = artwork_get(tabs[i].source)->name;
		float x = tab_x(i), w = (float)ui_text_width(FONT_SMALL, name) + 32.0f;
		int on = i == current;
		ui_rrect(x, TABS_Y + dy, w, TAB_H, TAB_H * 0.5f, ui_alpha(on ? C_ACCENT2 : C_SURFACE2, a));
		ui_text_center(FONT_SMALL, x + w * 0.5f, TABS_Y + dy + 23.0f, ui_alpha(on ? C_ON_ACCENT : C_TEXT2, a), name);
	}

	SourceTab *t = &tabs[current];
	const ArtResults *r = shown();
	if (t->searching) {
		ui_spinner(SCREEN_W * 0.5f, (GRID_Y + GRID_BOTTOM) * 0.5f, 18.0f, ui_alpha(C_TEXT2, a));
	} else if (t->err[0] && (!r || !r->count)) {
		draw_message(a, "That didn't work", t->err);
	} else if (r && !r->count) {
		draw_message(a, "Nothing found", "Try another name with \xE2\x96\xB3, or another source with L and R.");
	} else if (r) {
		float cw = card_w(), ih = image_h(), ch = ih + LABEL_H;
		int cols = columns();
		vita2d_enable_clipping();
		vita2d_set_clip_rectangle(0, (int)GRID_Y - 4, SCREEN_W, (int)GRID_BOTTOM + 4);
		for (int i = 0; i < r->count; i++) {
			float x = MARGIN + (float)(i % cols) * (cw + GAP);
			float y = GRID_Y + dy + (float)(i / cols) * (ch + GAP) - scroll;
			if (y + ch < GRID_Y - 4 || y > GRID_BOTTOM + 4)
				continue;
			const ArtResult *it = &r->items[i];
			ui_rrect(x, y, cw, ch, 10.0f, ui_alpha(C_SURFACE, a));
			ImageRef img = images_url(it->thumb[0] ? it->thumb : it->full, IMAGES_CARD_WIDTH);
			if (img.tex) {
				// Fill the card's picture area, cropping what overflows.
				float tw = (float)vita2d_texture_get_width(img.tex), th = (float)vita2d_texture_get_height(img.tex);
				float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
				if (tw / th > cw / ih) {
					float keep = (cw / ih) / (tw / th);
					u0 = (1.0f - keep) * 0.5f, u1 = 1.0f - u0;
				} else {
					float keep = (tw / th) / (cw / ih);
					v0 = (1.0f - keep) * 0.5f, v1 = 1.0f - v0;
				}
				ui_image_part(img.tex, x, y, cw, ih, u0, v0, u1, v1, 10, 10, 0, 0, ui_alpha(0xFFFFFFFF, a * img.alpha));
			} else if (!img.failed) {
				ui_spinner(x + cw * 0.5f, y + ih * 0.5f, 10.0f, ui_alpha(C_TEXT3, a));
			}
			char label[96], fit[96];
			if (it->w && it->h)
				snprintf(label, sizeof(label), "%s \xC2\xB7 %d\xC3\x97%d", it->label, it->w, it->h);
			else
				snprintf(label, sizeof(label), "%s", it->label);
			ui_text_fit(FONT_SMALL, label, (int)cw - 16, fit, sizeof(fit));
			ui_text(FONT_SMALL, x + 8.0f, y + ih + 26.0f, ui_alpha(C_TEXT2, a), fit);
			if (i == selected)
				ui_rrect_outline(x - 3.0f, y - 3.0f, cw + 6.0f, ch + 6.0f, 13.0f, 2.0f,
				                 ui_alpha(C_ACCENT, a * (0.75f + 0.25f * sinf(ui_time() * 4.0f))));
		}
		vita2d_disable_clipping();
	}

	Hint hints[4] = {{ui_back_glyph(), "Back"}, {ui_ok_glyph(), "Use"}, {GLYPH_TRIANGLE, "Search"}, {GLYPH_R, "Source"}};
	app_footer(hints, tab_count > 1 ? 4 : 3, artwork_get(t->source)->credit);
}
