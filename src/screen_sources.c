// Settings > Artwork sources: the sites Imago can search for app artwork, and
// their API keys (typed, or read from a QR code with the camera).
#include "app.h"
#include "artwork.h"
#include "dialog.h"
#include "ime.h"
#include "keys.h"
#include "qrscan.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIST_Y 92.0f
#define ROW_H 54.0f
#define ROW_GAP 6.0f
#define LIST_BOTTOM 470.0f

enum { STATUS_UNKNOWN, STATUS_TESTING, STATUS_OK, STATUS_FAILED };

static int is_open, closing, focus;
static float appear, scroll, scroll_target;
static int status[16];
static int menu_source;

// Typing keys: one keyboard per key the source needs, in order.
static int typing = -1;
static char typed[4][KEY_VALUE_MAX];

static int scanning;

void sources_open(void) {
	is_open = 1;
	closing = 0;
	appear = 0.0f;
}

int sources_is_open(void) {
	return is_open;
}

static int key_count(const ArtSource *s) {
	int n = 0;
	while (s->keys && s->keys[n])
		n++;
	return n;
}

static int has_any_key(const ArtSource *s) {
	for (int i = 0; s->keys && s->keys[i]; i++)
		if (keys_has(s->keys[i]))
			return 1;
	return 0;
}

// --- testing ---------------------------------------------------------------------

typedef struct {
	int source;
	int rc;
	int announce;
	char err[ART_ERROR_SIZE];
} TestJob;

static void test_work(void *ctx, NetClient *net) {
	TestJob *t = ctx;
	t->rc = artwork_get(t->source)->test(net, t->err, sizeof(t->err));
}

static void test_done(void *ctx) {
	TestJob *t = ctx;
	const ArtSource *s = artwork_get(t->source);
	status[t->source] = t->rc == 0 ? STATUS_OK : STATUS_FAILED;
	char text[ART_ERROR_SIZE + 64];
	if (t->rc == 0) {
		snprintf(text, sizeof(text), "%s works", s->name);
		app_toast(text);
	} else {
		snprintf(text, sizeof(text), "%s didn't work", s->name);
		dialog_message(text, t->err);
	}
	free(t);
}

static void test_source(int index) {
	TestJob *t = calloc(1, sizeof(*t));
	if (!t)
		return;
	t->source = index;
	status[index] = STATUS_TESTING;
	jobs_submit(test_work, test_done, t);
}

// --- entering keys ---------------------------------------------------------------

static void ask_next_key(void) {
	const ArtSource *s = artwork_get(menu_source);
	keys_get(s->keys[typing], typed[typing], sizeof(typed[0]));
	ime_open(s->key_prompts[typing], typed[typing], KEY_VALUE_MAX - 1);
}

static void start_typing(void) {
	typing = 0;
	memset(typed, 0, sizeof(typed));
	ask_next_key();
}

static void finish_typing(void) {
	const ArtSource *s = artwork_get(menu_source);
	for (int i = 0; s->keys[i]; i++)
		keys_set(s->keys[i], typed[i]);
	typing = -1;
	status[menu_source] = STATUS_UNKNOWN;
	if (artwork_ready(s))
		test_source(menu_source);
}

static void start_scanning(void) {
	char err[160];
	if (qrscan_open(err, sizeof(err)) != 0) {
		dialog_message("No camera", err);
		return;
	}
	scanning = 1;
}

static void stop_scanning(void) {
	qrscan_close();
	scanning = 0;
}

static void on_qr(const char *text) {
	stop_scanning();
	const ArtSource *s = artwork_get(menu_source);
	char msg[200];
	if (strchr(text, '=')) {
		// "name=value" lines: may set keys of several sources at once.
		int n = keys_import(text, artwork_key_names());
		if (n == 0) {
			dialog_message("Nothing to use in that code",
			               "Its lines don't name a key Imago knows. Use a bare key, or lines like "
			               "steamgriddb=YOUR_KEY.");
			return;
		}
		snprintf(msg, sizeof(msg), "Saved %d key%s from the code", n, n == 1 ? "" : "s");
		app_toast(msg);
		memset(status, 0, sizeof(status));
		for (int i = 0; i < artwork_count(); i++)
			if (artwork_get(i)->keys && artwork_ready(artwork_get(i)))
				test_source(i);
		return;
	}
	if (key_count(s) != 1) {
		snprintf(msg, sizeof(msg),
		         "%s needs %d values, and the code holds one. Put each on its own line, as name=value "
		         "(the names are in \"Where do I get a key?\").",
		         s->name, key_count(s));
		dialog_message("That code isn't enough", msg);
		return;
	}
	keys_set(s->keys[0], text);
	snprintf(msg, sizeof(msg), "Saved the %s key", s->name);
	app_toast(msg);
	test_source(menu_source);
}

// --- menus -----------------------------------------------------------------------

enum { ACT_SCAN, ACT_TYPE, ACT_TEST, ACT_REMOVE, ACT_HELP };
static int menu_actions[5], menu_action_count;

static void on_remove(int choice) {
	if (choice != 0)
		return;
	const ArtSource *s = artwork_get(menu_source);
	for (int i = 0; s->keys[i]; i++)
		keys_set(s->keys[i], "");
	status[menu_source] = STATUS_UNKNOWN;
	app_toast("Key removed");
}

static void on_menu(int choice) {
	if (choice < 0 || choice >= menu_action_count)
		return;
	const ArtSource *s = artwork_get(menu_source);
	switch (menu_actions[choice]) {
	case ACT_SCAN: start_scanning(); break;
	case ACT_TYPE: start_typing(); break;
	case ACT_TEST: test_source(menu_source); break;
	case ACT_REMOVE:
		dialog_confirm("Remove the key?", "Imago forgets it. You can add it again any time.", "Remove", 1, on_remove);
		break;
	case ACT_HELP: dialog_message(s->name, s->help); break;
	}
}

static void activate(int index) {
	const ArtSource *s = artwork_get(index);
	menu_source = index;
	static const char *labels[5];
	menu_action_count = 0;
	if (s->keys) {
		menu_actions[menu_action_count] = ACT_SCAN;
		labels[menu_action_count++] = "Scan a QR code";
		menu_actions[menu_action_count] = ACT_TYPE;
		labels[menu_action_count++] = key_count(s) > 1 ? "Type the keys" : "Type the key";
	}
	if (artwork_ready(s)) {
		menu_actions[menu_action_count] = ACT_TEST;
		labels[menu_action_count++] = "Test the connection";
	}
	if (s->keys && has_any_key(s)) {
		menu_actions[menu_action_count] = ACT_REMOVE;
		labels[menu_action_count++] = key_count(s) > 1 ? "!Remove the keys" : "!Remove the key";
	}
	menu_actions[menu_action_count] = ACT_HELP;
	labels[menu_action_count++] = s->keys ? "Where do I get a key?" : "About this source";
	dialog_menu(s->name, labels, menu_action_count, 0, on_menu);
}

// --- input & drawing -------------------------------------------------------------

void sources_update(const Input *in) {
	if (closing) {
		appear = ui_approach(appear, 0.0f, 20.0f);
		if (appear < 0.02f)
			is_open = 0;
		return;
	}
	appear = ui_approach(appear, 1.0f, 16.0f);
	if (typing >= 0) {
		if (!ime_active()) { // the keyboard couldn't open
			typing = -1;
			return;
		}
		char text[KEY_VALUE_MAX];
		int r = ime_poll(text, sizeof(text));
		if (r == 1) {
			const ArtSource *s = artwork_get(menu_source);
			snprintf(typed[typing], sizeof(typed[0]), "%s", text);
			if (s->keys[++typing])
				ask_next_key();
			else
				finish_typing();
		} else if (r == -1) {
			typing = -1;
		}
		return;
	}
	if (scanning) {
		char text[KEY_VALUE_MAX * 4];
		if (qrscan_poll(text, sizeof(text)))
			on_qr(text);
		else if ((in->pressed & BTN_BACK) || in->tapped)
			stop_scanning();
		return;
	}
	int count = artwork_count();
	if ((in->repeat & SCE_CTRL_UP) && focus > 0)
		focus--;
	if ((in->repeat & SCE_CTRL_DOWN) && focus < count - 1)
		focus++;
	if (in->pressed & BTN_OK)
		activate(focus);
	if (in->pressed & BTN_BACK)
		closing = 1;
	if (in->tapped) {
		if (in->tap_y < HEADER_H && in->tap_x < 200) {
			closing = 1;
		} else if (in->tap_y >= LIST_Y && in->tap_y < LIST_BOTTOM) {
			int i = (int)((in->tap_y - LIST_Y + scroll) / (ROW_H + ROW_GAP));
			if (i >= 0 && i < count) {
				focus = i;
				activate(i);
			}
		}
	}
	// Keep the focused row in view.
	float top = (float)focus * (ROW_H + ROW_GAP), bottom = top + ROW_H;
	if (top < scroll_target)
		scroll_target = top;
	if (bottom > scroll_target + (LIST_BOTTOM - LIST_Y))
		scroll_target = bottom - (LIST_BOTTOM - LIST_Y);
	scroll = ui_approach(scroll, scroll_target, 14.0f);
}

static void row_status(int index, char *out, size_t size, uint32_t *color) {
	const ArtSource *s = artwork_get(index);
	*color = C_TEXT3;
	if (status[index] == STATUS_TESTING) {
		snprintf(out, size, "Testing...");
	} else if (!s->keys) {
		snprintf(out, size, status[index] == STATUS_FAILED ? "No key needed \xC2\xB7 didn't connect" : "No key needed");
	} else if (!artwork_ready(s)) {
		snprintf(out, size, "Not set up");
	} else if (status[index] == STATUS_OK) {
		snprintf(out, size, "Works");
		*color = C_SUCCESS;
	} else if (status[index] == STATUS_FAILED) {
		snprintf(out, size, "Didn't connect");
		*color = C_DANGER;
	} else {
		snprintf(out, size, "Key saved");
	}
}

static void draw_scanner(float a) {
	ui_rect(0, 0, SCREEN_W, SCREEN_H, ui_alpha(C_BG, a));
	float h = 400.0f, w = h * QRSCAN_W / QRSCAN_H, x = (SCREEN_W - w) * 0.5f, y = 44.0f;
	vita2d_texture *tex = qrscan_preview();
	if (tex)
		ui_image(tex, x, y, w, h, 12, 12, 12, 12, ui_alpha(0xFFFFFFFF, a));
	else
		ui_rrect(x, y, w, h, 12.0f, ui_alpha(C_SURFACE, a));
	// A square guide in the middle, where the code should go.
	float g = 240.0f;
	ui_rrect_outline(SCREEN_W * 0.5f - g * 0.5f, y + (h - g) * 0.5f, g, g, 16.0f, 3.0f,
	                 ui_alpha(C_ACCENT, a * (0.7f + 0.3f * sinf(ui_time() * 4.0f))));
	if (!tex)
		ui_spinner(SCREEN_W * 0.5f, y + h * 0.5f, 18.0f, ui_alpha(C_TEXT2, a));
	char text[128];
	snprintf(text, sizeof(text), "Point the back camera at the QR code for %s", artwork_get(menu_source)->name);
	ui_text_center(FONT_BODY, SCREEN_W * 0.5f, y + h + 30.0f, ui_alpha(C_TEXT, a), text);
	Hint hints[1] = {{ui_back_glyph(), "Cancel"}};
	app_footer(hints, 1, NULL);
}

void sources_draw(void) {
	if (!is_open)
		return;
	float a = appear, dy = (1.0f - a) * 20.0f;
	if (scanning) {
		draw_scanner(a);
		return;
	}
	ui_rect(0, 0, SCREEN_W, SCREEN_H, ui_alpha(C_BG, a));
	ui_vgradient(0, 0, SCREEN_W, 160.0f, ui_alpha(C_ACCENT2, 0.10f * a), ui_alpha(C_ACCENT2, 0.0f));
	ui_glyph(ui_back_glyph(), MARGIN + 10.0f, 34.0f, a);
	ui_text(FONT_BOLD, MARGIN + 28.0f, 40.0f, ui_alpha(C_TEXT2, a), "Back");
	ui_text_right(FONT_HEADING, SCREEN_W - MARGIN, 44.0f, ui_alpha(C_TEXT, a), "Artwork sources");

	int count = artwork_count();
	vita2d_enable_clipping();
	vita2d_set_clip_rectangle(0, (int)LIST_Y - 4, SCREEN_W, (int)LIST_BOTTOM + 4);
	for (int i = 0; i < count; i++) {
		float x = MARGIN, w = SCREEN_W - 2 * MARGIN, y = LIST_Y + dy + (float)i * (ROW_H + ROW_GAP) - scroll;
		if (y + ROW_H < LIST_Y - 4 || y > LIST_BOTTOM + 4)
			continue;
		const ArtSource *s = artwork_get(i);
		ui_rrect(x, y, w, ROW_H, 12.0f, ui_alpha(i == focus ? C_SURFACE2 : C_SURFACE, a));
		ui_text(FONT_BOLD, x + 16.0f, y + 33.0f, ui_alpha(C_TEXT, a), s->name);
		char value[80];
		uint32_t color;
		row_status(i, value, sizeof(value), &color);
		if (status[i] == STATUS_TESTING)
			ui_spinner(x + w - 28.0f, y + ROW_H * 0.5f, 9.0f, ui_alpha(C_TEXT2, a));
		else
			ui_text_right(FONT_SMALL, x + w - 16.0f, y + 32.0f, ui_alpha(color, a), value);
		if (i == focus)
			ui_rrect_outline(x - 3.0f, y - 3.0f, w + 6.0f, ROW_H + 6.0f, 15.0f, 2.0f,
			                 ui_alpha(C_ACCENT, a * (0.75f + 0.25f * sinf(ui_time() * 4.0f))));
	}
	vita2d_disable_clipping();
	ui_text(FONT_SMALL, MARGIN, LIST_BOTTOM + 26.0f, ui_alpha(C_TEXT3, a),
	        "Keys stay on this memory card, in " KEYS_PATH ".");

	Hint hints[2] = {{ui_back_glyph(), "Back"}, {ui_ok_glyph(), "Select"}};
	app_footer(hints, 2, NULL);
}
