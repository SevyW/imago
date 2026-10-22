// Theme details: large preview, metadata, tags and the install action.
#include "app.h"
#include "dialog.h"
#include "images.h"
#include "jobs.h"
#include "log.h"
#include "tasks.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PREVIEW_X 32.0f
#define PREVIEW_Y 72.0f
#define PREVIEW_W 576.0f
#define PREVIEW_H 326.0f
#define PREVIEW_WIDTH 576 // requested image width
#define COL_X 640.0f
#define COL_W (SCREEN_W - COL_X - 32.0f)
#define TAG_Y 418.0f
#define TAG_H 30.0f
#define BUTTON_Y 384.0f
#define BUTTON_H 50.0f
#define AUTHOR_Y 444.0f
#define AUTHOR_H 40.0f

enum { FOCUS_INSTALL, FOCUS_AUTHOR, FOCUS_TAGS };

static int is_open, closing;
static float appear;
static PsvtTheme theme;
static PsvtDetail detail;
static int detail_state; // 0 loading, 1 loaded, -1 failed
static int generation;
static int focus;
static float tag_x[PSVT_MAX_TAGS], tag_w[PSVT_MAX_TAGS], tag_row[PSVT_MAX_TAGS];
static int tags_shown;

typedef struct {
	int generation;
	uint32_t id;
	PsvtDetail detail;
	int rc;
} DetailJob;

static void detail_work(void *ctx, NetClient *net) {
	DetailJob *job = ctx;
	char url[128];
	psvt_detail_url(url, sizeof(url), job->id);
	char *html = NULL;
	// The page carries megabytes of inline images after the metadata; stop
	// reading at the marker so this costs ~80 KB instead of ~2.5 MB.
	job->rc = net_get(net, url, &html, NULL, 8 * 1024 * 1024, PSVT_DETAIL_END_MARKER, NULL);
	if (job->rc == NET_OK)
		job->rc = psvt_parse_detail(html, &job->detail);
	free(html);
}

static void detail_done(void *ctx) {
	DetailJob *job = ctx;
	if (job->generation == generation) {
		if (job->rc == 0) {
			detail = job->detail;
			detail_state = 1;
		} else {
			detail_state = -1;
		}
	}
	free(job);
}

void detail_open(const PsvtTheme *t) {
	log_write("detail: opened %u \"%s\"", (unsigned)t->id, t->title);
	theme = *t;
	memset(&detail, 0, sizeof(detail));
	detail_state = 0;
	is_open = 1;
	closing = 0;
	appear = 0.0f;
	focus = FOCUS_INSTALL;
	generation++;
	DetailJob *job = calloc(1, sizeof(*job));
	if (job) {
		job->generation = generation;
		job->id = t->id;
		jobs_submit(detail_work, detail_done, job);
	}
}

int detail_is_open(void) {
	return is_open;
}

static void close_detail(void) {
	closing = 1;
}

static void on_cancel_install(int choice) {
	Task task;
	if (choice == 0 && tasks_find(theme.id, &task))
		tasks_cancel(task.uid);
}

static void activate_install(void) {
	Task task;
	if (tasks_find(theme.id, &task) && task.state <= TASK_FINISHING) {
		if (task.state != TASK_FINISHING)
			dialog_confirm("Cancel installation?", theme.title, "Cancel install", 1, on_cancel_install);
		return;
	}
	if (!g_db_writable) {
		dialog_message("Unsafe homebrew is disabled",
		               "Imago needs write access to the system theme list. Open HENkaku Settings "
		               "(molecularShell or VitaShell > START), enable \"Unsafe homebrew\", then restart Imago.");
		return;
	}
	const char *hash = detail_state == 1 && detail.hash[0] ? detail.hash : theme.hash;
	tasks_install(theme.id, hash, theme.title);
}

static void filter_by(const char *tag, const char *author) {
	discover_set_filter(tag, author);
	app_switch_tab(TAB_DISCOVER);
	close_detail();
}

static void activate(int target) {
	if (target == FOCUS_INSTALL)
		activate_install();
	else if (target == FOCUS_AUTHOR && detail.author[0])
		filter_by(NULL, detail.author);
	else if (target >= FOCUS_TAGS && target - FOCUS_TAGS < tags_shown)
		filter_by(detail.tags[target - FOCUS_TAGS], NULL);
}

static int hit(int x, int y) {
	if (x >= COL_X && x <= COL_X + COL_W) {
		if (y >= BUTTON_Y && y <= BUTTON_Y + BUTTON_H)
			return FOCUS_INSTALL;
		if (y >= AUTHOR_Y && y <= AUTHOR_Y + AUTHOR_H)
			return FOCUS_AUTHOR;
	}
	for (int i = 0; i < tags_shown; i++)
		if (x >= tag_x[i] && x <= tag_x[i] + tag_w[i] && y >= tag_row[i] && y <= tag_row[i] + TAG_H)
			return FOCUS_TAGS + i;
	return -1;
}

void detail_update(const Input *in) {
	if (closing) {
		appear = ui_approach(appear, 0.0f, 20.0f);
		if (appear < 0.02f)
			is_open = 0;
		return;
	}
	appear = ui_approach(appear, 1.0f, 16.0f);

	int has_author = detail.author[0] != 0;
	if (in->repeat & SCE_CTRL_UP) {
		if (focus == FOCUS_AUTHOR)
			focus = FOCUS_INSTALL;
		else if (focus >= FOCUS_TAGS)
			focus = FOCUS_INSTALL;
	}
	if (in->repeat & SCE_CTRL_DOWN) {
		if (focus == FOCUS_INSTALL && has_author)
			focus = FOCUS_AUTHOR;
	}
	if (in->repeat & SCE_CTRL_LEFT) {
		if (focus < FOCUS_TAGS && tags_shown > 0)
			focus = FOCUS_TAGS + tags_shown - 1;
		else if (focus > FOCUS_TAGS)
			focus--;
	}
	if (in->repeat & SCE_CTRL_RIGHT) {
		if (focus >= FOCUS_TAGS && focus < FOCUS_TAGS + tags_shown - 1)
			focus++;
		else if (focus >= FOCUS_TAGS)
			focus = FOCUS_INSTALL;
	}
	if (in->pressed & BTN_OK)
		activate(focus);
	else if (in->pressed & SCE_CTRL_TRIANGLE && has_author)
		activate(FOCUS_AUTHOR);
	else if (in->pressed & BTN_BACK)
		close_detail();
	if (in->tapped) {
		int target = hit(in->tap_x, in->tap_y);
		if (target >= 0) {
			focus = target;
			activate(target);
		} else if (in->tap_y < HEADER_H) {
			close_detail();
		}
	}
}

// "21 October 2026" -> "21 Oct 2026" so it fits the stat column.
static void short_date(const char *date, char *out, size_t size) {
	int day, year;
	char month[16];
	if (sscanf(date, "%d %15s %d", &day, month, &year) == 3)
		snprintf(out, size, "%d %.3s %d", day, month, year);
	else
		snprintf(out, size, "%s", date[0] ? date : "-");
}

// "100,00%" -> "100%"; keeps meaningful decimals ("97,50%").
static void short_score(const char *score, char *out, size_t size) {
	snprintf(out, size, "%s", score[0] ? score : "-");
	char *zeros = strstr(out, ",00%");
	if (!zeros)
		zeros = strstr(out, ".00%");
	if (zeros)
		memmove(zeros, zeros + 3, strlen(zeros + 3) + 1);
}

static void draw_stat(float x, float y, const char *label, const char *value, uint32_t color) {
	ui_text(FONT_SMALL, x, y, C_TEXT3, label);
	char fitted[64];
	ui_text_fit(FONT_BOLD, value, (int)(COL_W * 0.5f) - 20, fitted, sizeof(fitted));
	ui_text(FONT_BOLD, x, y + 22.0f, color, fitted);
}

static void focus_ring(float x, float y, float w, float h, float r, float a) {
	ui_rrect_outline(x - 4, y - 4, w + 8, h + 8, r + 4, 2.5f, ui_alpha(C_ACCENT, a));
}

void detail_draw(void) {
	if (!is_open)
		return;
	float a = appear;
	float dy = (1.0f - a) * 20.0f;

	// Ambient backdrop: the small preview stretched full-screen reads as a blur.
	ui_rect(0, 0, SCREEN_W, SCREEN_H, ui_alpha(C_BG, a));
	ImageRef small = images_remote(theme.hash, 216);
	if (small.tex)
		ui_image(small.tex, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 0, RGBA8(255, 255, 255, (int)(70 * a * small.alpha)));
	ui_vgradient(0, 0, SCREEN_W, SCREEN_H, ui_alpha(C_BG, 0.55f * a), ui_alpha(C_BG, 0.97f * a));

	// Top bar
	ui_glyph(ui_back_glyph(), MARGIN + 10.0f, 34.0f, a);
	ui_text(FONT_BOLD, MARGIN + 28.0f, 40.0f, ui_alpha(C_TEXT2, a), "Back");

	// Preview: show the cached thumbnail immediately, the sharp one when ready.
	float py = PREVIEW_Y + dy;
	ui_shadow(PREVIEW_X, py, PREVIEW_W, PREVIEW_H, 16.0f, a * 1.4f);
	ui_rrect(PREVIEW_X, py, PREVIEW_W, PREVIEW_H, 16.0f, ui_alpha(C_SURFACE2, a));
	ImageRef big = images_remote(theme.hash, PREVIEW_WIDTH);
	if (small.tex && (!big.tex || big.alpha < 1.0f))
		ui_image(small.tex, PREVIEW_X, py, PREVIEW_W, PREVIEW_H, 16, 16, 16, 16, RGBA8(255, 255, 255, (int)(255 * a)));
	if (big.tex)
		ui_image(big.tex, PREVIEW_X, py, PREVIEW_W, PREVIEW_H, 16, 16, 16, 16,
		         RGBA8(255, 255, 255, (int)(255 * a * big.alpha)));
	else if (!small.tex)
		ui_spinner(PREVIEW_X + PREVIEW_W * 0.5f, py + PREVIEW_H * 0.5f, 14.0f, ui_alpha(C_ACCENT, a));

	// Tags
	tags_shown = 0;
	if (detail_state == 1) {
		float tx = PREVIEW_X, ty = TAG_Y + dy;
		for (int i = 0; i < detail.tag_count && ty < FOOTER_Y - TAG_H; i++) {
			char label[64];
			snprintf(label, sizeof(label), "#%s", detail.tags[i]);
			float w = (float)ui_text_width(FONT_BODY, label) + 26.0f;
			if (tx + w > PREVIEW_X + PREVIEW_W) {
				tx = PREVIEW_X;
				ty += TAG_H + 10.0f;
				if (ty > FOOTER_Y - TAG_H - 4.0f)
					break;
			}
			int focused = focus == FOCUS_TAGS + i;
			ui_rrect(tx, ty, w, TAG_H, TAG_H * 0.5f, ui_alpha(focused ? C_SURFACE3 : C_SURFACE, a));
			ui_text(FONT_BODY, tx + 13.0f, ty + 20.5f, ui_alpha(focused ? C_TEXT : C_TEXT2, a), label);
			if (focused)
				focus_ring(tx, ty, w, TAG_H, TAG_H * 0.5f, a);
			tag_x[i] = tx;
			tag_w[i] = w;
			tag_row[i] = ty;
			tags_shown++;
			tx += w + 8.0f;
		}
	} else if (detail_state == 0) {
		for (int i = 0; i < 4; i++)
			ui_rrect(PREVIEW_X + (float)i * 104.0f, TAG_Y + dy, 94.0f, TAG_H, TAG_H * 0.5f, ui_alpha(C_SURFACE, a));
	}
	if (focus >= FOCUS_TAGS && focus - FOCUS_TAGS >= tags_shown)
		focus = FOCUS_INSTALL;

	// Title and author
	float y = PREVIEW_Y + 24.0f + dy;
	int lines = ui_text_wrap(FONT_HEADING, COL_X, y, (int)COL_W, 32, 3, ui_alpha(C_TEXT, a), theme.title);
	y += (float)(lines - 1) * 32.0f + 30.0f;
	if (detail_state == 1 && detail.author[0]) {
		char by[96];
		snprintf(by, sizeof(by), "by %s", detail.author);
		char fitted[96];
		ui_text_fit(FONT_BODY, by, (int)COL_W, fitted, sizeof(fitted));
		ui_text(FONT_BODY, COL_X, y, ui_alpha(C_ACCENT, a), fitted);
	} else if (detail_state == 0) {
		ui_rrect(COL_X, y - 13.0f, 120.0f, 14.0f, 7.0f, ui_alpha(C_SURFACE2, a));
	}

	// Stats
	float sy = y + 24.0f;
	float sh = 128.0f;
	ui_rrect(COL_X, sy, COL_W, sh, 14.0f, ui_alpha(C_SURFACE, a * 0.92f));
	char downloads[32], value[64];
	int dl = detail_state == 1 && detail.downloads >= 0 ? detail.downloads : theme.downloads;
	app_format_count(downloads, sizeof(downloads), dl);
	float c1 = COL_X + 18.0f, c2 = COL_X + COL_W * 0.5f + 6.0f;
	draw_stat(c1, sy + 28.0f, "Downloads", dl >= 0 ? downloads : "-", ui_alpha(C_TEXT, a));
	draw_stat(c2, sy + 28.0f, "Version", detail_state == 1 ? detail.version : "-", ui_alpha(C_TEXT, a));
	if (detail_state == 1) {
		int clean = detail.warnings == 0 && detail.errors == 0;
		short_score(detail.score, value, sizeof(value));
		draw_stat(c1, sy + 84.0f, "Validation", value, ui_alpha(clean ? C_SUCCESS : C_WARNING, a));
		short_date(detail.added, value, sizeof(value));
		draw_stat(c2, sy + 84.0f, "Added", value, ui_alpha(C_TEXT, a));
	} else {
		draw_stat(c1, sy + 84.0f, "Validation", theme.valid ? "100%" : "-", ui_alpha(C_TEXT, a));
		draw_stat(c2, sy + 84.0f, "Added", detail_state < 0 ? "-" : "...", ui_alpha(C_TEXT, a));
	}

	// Primary action
	InstallButton b = app_install_button(&theme);
	float by = BUTTON_Y + dy;
	ui_rrect(COL_X, by, COL_W, BUTTON_H, 14.0f, ui_alpha(b.busy ? C_SURFACE2 : b.background, a));
	if (b.busy && b.progress >= 0.0f) {
		float w = COL_W * b.progress;
		if (w > 28.0f)
			ui_rrect(COL_X, by, w, BUTTON_H, 14.0f, ui_alpha(C_ACCENT, a * 0.55f));
	}
	if (b.busy)
		ui_spinner(COL_X + 26.0f, by + BUTTON_H * 0.5f, 8.0f, ui_alpha(C_TEXT, a));
	ui_text_center(FONT_TITLE, COL_X + COL_W * 0.5f, by + 32.0f, ui_alpha(C_ON_ACCENT, a), b.label);
	if (focus == FOCUS_INSTALL)
		focus_ring(COL_X, by, COL_W, BUTTON_H, 14.0f, a);

	// Secondary action
	if (detail.author[0]) {
		float ay = AUTHOR_Y + dy;
		ui_rrect(COL_X, ay, COL_W, AUTHOR_H, 12.0f, ui_alpha(C_SURFACE, a));
		char label[96], fitted[96];
		snprintf(label, sizeof(label), "More by %s", detail.author);
		ui_text_fit(FONT_BOLD, label, (int)COL_W - 60, fitted, sizeof(fitted));
		ui_glyph(GLYPH_TRIANGLE, COL_X + 22.0f, ay + AUTHOR_H * 0.5f, a);
		ui_text(FONT_BOLD, COL_X + 40.0f, ay + 26.0f, ui_alpha(C_TEXT2, a), fitted);
		if (focus == FOCUS_AUTHOR)
			focus_ring(COL_X, ay, COL_W, AUTHOR_H, 12.0f, a);
	} else if (detail_state == -1) {
		ui_text(FONT_SMALL, COL_X, AUTHOR_Y + 22.0f + dy, ui_alpha(C_TEXT3, a), "Couldn't load all details.");
	}

	Hint hints[] = {{ui_back_glyph(), "Back"}, {ui_ok_glyph(), "Select"}};
	app_footer(hints, 2, NULL);
}
