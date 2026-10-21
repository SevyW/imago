#include "dialog.h"

#include "ui.h"

#include <stdio.h>
#include <string.h>

#define MAX_OPTIONS 40
#define VISIBLE_ROWS 7 // longer menus scroll
#define WIDTH 540.0f
#define PAD 26.0f
#define ROW_H 46.0f
#define BUTTON_H 44.0f

typedef enum { KIND_NONE, KIND_MENU, KIND_CONFIRM, KIND_MESSAGE } Kind;

static Kind kind;
static char title[128];
static char message[512];
static char options[MAX_OPTIONS][64];
static int option_count, focus;
static int first_row; // first menu row shown
static int destructive;
static DialogFn callback;
static float appear; // 0..1 open animation
static int closing;
static int result;

static void open_dialog(Kind k, const char *t, DialogFn fn) {
	kind = k;
	snprintf(title, sizeof(title), "%s", t ? t : "");
	message[0] = '\0';
	option_count = 0;
	focus = 0;
	first_row = 0;
	destructive = 0;
	callback = fn;
	appear = 0.0f;
	closing = 0;
}

void dialog_menu(const char *t, const char *const *opts, int count, int selected, DialogFn fn) {
	open_dialog(KIND_MENU, t, fn);
	option_count = count < MAX_OPTIONS ? count : MAX_OPTIONS;
	for (int i = 0; i < option_count; i++)
		snprintf(options[i], sizeof(options[i]), "%s", opts[i]);
	focus = selected >= 0 && selected < option_count ? selected : 0;
	first_row = focus >= VISIBLE_ROWS ? focus - VISIBLE_ROWS + 1 : 0;
}

void dialog_confirm(const char *t, const char *msg, const char *confirm_label, int is_destructive,
                    DialogFn fn) {
	open_dialog(KIND_CONFIRM, t, fn);
	snprintf(message, sizeof(message), "%s", msg);
	snprintf(options[0], sizeof(options[0]), "Cancel");
	snprintf(options[1], sizeof(options[1]), "%s", confirm_label);
	option_count = 2;
	destructive = is_destructive;
	focus = is_destructive ? 0 : 1;
}

void dialog_message(const char *t, const char *msg) {
	open_dialog(KIND_MESSAGE, t, NULL);
	snprintf(message, sizeof(message), "%s", msg);
	snprintf(options[0], sizeof(options[0]), "OK");
	option_count = 1;
}

int dialog_open(void) {
	return kind != KIND_NONE;
}

static void close_with(int choice) {
	closing = 1;
	result = choice;
}

// --- layout -------------------------------------------------------------------

static float message_height(void) {
	if (!message[0])
		return 0.0f;
	int lines = ui_text_wrap_lines(FONT_BODY, (int)(WIDTH - 2 * PAD), message);
	return (float)(lines * 23) + 10.0f;
}

static int visible_rows(void) {
	return option_count < VISIBLE_ROWS ? option_count : VISIBLE_ROWS;
}

static float body_height(void) {
	float h = PAD + 30.0f + message_height();
	if (kind == KIND_MENU)
		h += (float)visible_rows() * ROW_H + 6.0f;
	else
		h += 18.0f + BUTTON_H;
	return h + PAD;
}

static void card_rect(float *x, float *y, float *h) {
	*h = body_height();
	*x = (SCREEN_W - WIDTH) * 0.5f;
	*y = (SCREEN_H - *h) * 0.5f + (1.0f - appear) * 24.0f;
}

static void button_rect(int index, float cx, float cy, float ch, float *bx, float *by, float *bw) {
	*bw = (WIDTH - 2 * PAD - 12.0f) / (float)option_count;
	*bx = cx + PAD + (float)index * (*bw + 12.0f);
	*by = cy + ch - PAD - BUTTON_H;
}

static int hit_option(int tx, int ty) {
	float cx, cy, ch;
	card_rect(&cx, &cy, &ch);
	for (int i = 0; i < option_count; i++) {
		float x, y, w, h;
		if (kind == KIND_MENU) {
			if (i < first_row || i >= first_row + visible_rows())
				continue;
			x = cx + 12.0f;
			y = cy + PAD + 30.0f + message_height() + (float)(i - first_row) * ROW_H;
			w = WIDTH - 24.0f;
			h = ROW_H;
		} else {
			button_rect(i, cx, cy, ch, &x, &y, &w);
			h = BUTTON_H;
		}
		if (tx >= x && tx < x + w && ty >= y && ty < y + h)
			return i;
	}
	if (tx < cx || tx > cx + WIDTH || ty < cy || ty > cy + ch)
		return -2; // outside the card
	return -1;
}

static int choice_for(int index) {
	if (kind == KIND_CONFIRM)
		return index == 1 ? 0 : -1;
	if (kind == KIND_MESSAGE)
		return 0;
	return index;
}

void dialog_update(const Input *in) {
	if (kind == KIND_NONE)
		return;
	if (closing) {
		appear = ui_approach(appear, 0.0f, 22.0f);
		if (appear <= 0.02f) {
			DialogFn fn = callback;
			kind = KIND_NONE;
			if (fn)
				fn(result); // may open another dialog
		}
		return;
	}
	appear = ui_approach(appear, 1.0f, 18.0f);

	int vertical = kind == KIND_MENU;
	uint32_t prev = vertical ? SCE_CTRL_UP : SCE_CTRL_LEFT;
	uint32_t next = vertical ? SCE_CTRL_DOWN : SCE_CTRL_RIGHT;
	if ((in->repeat & prev) && focus > 0)
		focus--;
	if ((in->repeat & next) && focus < option_count - 1)
		focus++;
	if (focus < first_row)
		first_row = focus;
	if (focus >= first_row + VISIBLE_ROWS)
		first_row = focus - VISIBLE_ROWS + 1;
	if (in->pressed & BTN_OK)
		close_with(choice_for(focus));
	else if (in->pressed & BTN_BACK)
		close_with(-1);
	else if (in->tapped) {
		int hit = hit_option(in->tap_x, in->tap_y);
		if (hit >= 0) {
			focus = hit;
			close_with(choice_for(hit));
		} else if (hit == -2) {
			close_with(-1);
		}
	}
}

void dialog_draw(void) {
	if (kind == KIND_NONE)
		return;
	float a = appear;
	ui_rect(0, 0, SCREEN_W, SCREEN_H, ui_alpha(C_SCRIM, a));
	float cx, cy, ch;
	card_rect(&cx, &cy, &ch);
	ui_shadow(cx, cy, WIDTH, ch, 18.0f, a * 1.6f);
	ui_rrect(cx, cy, WIDTH, ch, 18.0f, ui_alpha(C_SURFACE, a));
	ui_rrect_outline(cx, cy, WIDTH, ch, 18.0f, 1.0f, ui_alpha(C_STROKE, a));

	float y = cy + PAD + 20.0f;
	char fitted[160];
	ui_text_fit(FONT_TITLE, title, (int)(WIDTH - 2 * PAD), fitted, sizeof(fitted));
	ui_text(FONT_TITLE, cx + PAD, y, ui_alpha(C_TEXT, a), fitted);
	y += 10.0f;
	if (message[0]) {
		ui_text_wrap(FONT_BODY, cx + PAD, y + 22.0f, (int)(WIDTH - 2 * PAD), 23, 0, ui_alpha(C_TEXT2, a), message);
		y += message_height();
	}

	if (kind == KIND_MENU) {
		y += 8.0f;
		for (int i = first_row; i < first_row + visible_rows(); i++) {
			float ry = y + (float)(i - first_row) * ROW_H;
			int danger = options[i][0] == '!';
			const char *label = danger ? options[i] + 1 : options[i];
			if (i == focus)
				ui_rrect(cx + 12.0f, ry, WIDTH - 24.0f, ROW_H - 4.0f, 11.0f, ui_alpha(C_SURFACE3, a));
			uint32_t color = danger ? C_DANGER : i == focus ? C_TEXT : C_TEXT2;
			ui_text(FONT_BOLD, cx + PAD + 4.0f, ry + 27.0f, ui_alpha(color, a), label);
			if (i == focus)
				ui_glyph(ui_ok_glyph(), cx + WIDTH - PAD - 12.0f, ry + (ROW_H - 4.0f) * 0.5f, a);
		}
		// Chevrons when there are more options above or below.
		float mx = cx + WIDTH * 0.5f;
		if (first_row > 0) {
			ui_line(mx - 8.0f, y - 2.0f, mx, y - 8.0f, 2.0f, ui_alpha(C_TEXT3, a));
			ui_line(mx, y - 8.0f, mx + 8.0f, y - 2.0f, 2.0f, ui_alpha(C_TEXT3, a));
		}
		if (first_row + visible_rows() < option_count) {
			float by = y + (float)visible_rows() * ROW_H + 2.0f;
			ui_line(mx - 8.0f, by, mx, by + 6.0f, 2.0f, ui_alpha(C_TEXT3, a));
			ui_line(mx, by + 6.0f, mx + 8.0f, by, 2.0f, ui_alpha(C_TEXT3, a));
		}
		return;
	}

	for (int i = 0; i < option_count; i++) {
		float bx, by, bw;
		button_rect(i, cx, cy, ch, &bx, &by, &bw);
		int primary = i == option_count - 1;
		uint32_t bg = primary ? (destructive ? C_DANGER : C_ACCENT) : C_SURFACE2;
		uint32_t fg = primary ? C_ON_ACCENT : C_TEXT;
		ui_rrect(bx, by, bw, BUTTON_H, 12.0f, ui_alpha(bg, a));
		if (i == focus)
			ui_rrect_outline(bx - 4, by - 4, bw + 8, BUTTON_H + 8, 15.0f, 2.0f, ui_alpha(C_TEXT, a * 0.9f));
		ui_text_center(FONT_BOLD, bx + bw * 0.5f, by + 28.0f, ui_alpha(fg, a), options[i]);
	}
}
