// Framing a picture for a part, with a live preview of the result: the
// LiveArea drawn where the shell draws it (with the start gate cut from the
// same picture when "seamless" is on), or the bubble on a home screen.
#include "appdb.h"
#include "dialog.h"
#include "edit.h"
#include "images.h"
#include "livearea.h"
#include "app.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN_ZOOM 0.08f
#define PAN_SPEED 0.9f  // crop widths per second at full stick
#define ZOOM_SPEED 1.1f // e-folds per second

// The home screen preview
#define BUBBLE_BIG 232.0f
#define BUBBLE_X 92.0f
#define BUBBLE_Y 128.0f
#define HOME_X 440.0f
#define HOME_Y 104.0f
#define HOME_W 470.0f
#define HOME_H 300.0f
#define HOME_BUBBLE 84.0f

typedef enum { VIEW_PREVIEW, VIEW_PICTURE } View;

static int is_open, closing;
static float appear;
static char app_id[16], title[128], source[256];
static ArtPart part;
static int target_w, target_h;
static float zoom, cx, cy; // crop size relative to the largest fitting crop; centre in 0..1
static View view;
static LiveArea la;
static int gate_x, gate_y;  // gate position inside the background
static int gate_known;      // gate_x/y measured for the app's layout
static int seamless;        // background: cut the gate from the same picture
static int can_seamless;    // ...possible for this app

// --- geometry -------------------------------------------------------------------

static int texture_size(float *sw, float *sh) {
	ImageRef img = images_file(source);
	if (!img.tex)
		return 0;
	*sw = (float)vita2d_texture_get_width(img.tex);
	*sh = (float)vita2d_texture_get_height(img.tex);
	return 1;
}

// The crop in fractions of the picture, with the target's aspect ratio.
static Crop current_crop(void) {
	Crop c = {0, 0, 1, 1};
	float sw, sh;
	if (!texture_size(&sw, &sh))
		return c;
	float aspect = (float)target_w / (float)target_h;
	float mw = 1.0f, mh = 1.0f;
	if (sw / sh > aspect)
		mw = sh * aspect / sw;
	else
		mh = sw / aspect / sh;
	c.w = mw * zoom;
	c.h = mh * zoom;
	c.x = cx - c.w * 0.5f;
	c.y = cy - c.h * 0.5f;
	return c;
}

static void clamp_crop(void) {
	if (zoom > 1.0f)
		zoom = 1.0f;
	if (zoom < MIN_ZOOM)
		zoom = MIN_ZOOM;
	Crop c = current_crop();
	float hw = (float)c.w * 0.5f, hh = (float)c.h * 0.5f;
	cx = cx < hw ? hw : cx > 1.0f - hw ? 1.0f - hw : cx;
	cy = cy < hh ? hh : cy > 1.0f - hh ? 1.0f - hh : cy;
}

// The part of the crop the gate shows, in fractions of the picture.
static void gate_uv(Crop c, float *u0, float *v0, float *u1, float *v1) {
	*u0 = (float)(c.x + c.w * gate_x / BG_W);
	*v0 = (float)(c.y + c.h * gate_y / BG_H);
	*u1 = (float)(c.x + c.w * (gate_x + GATE_W) / BG_W);
	*v1 = (float)(c.y + c.h * (gate_y + GATE_H) / BG_H);
}

// --- opening --------------------------------------------------------------------

void editor_open(const char *id, ArtPart p, const char *path) {
	snprintf(app_id, sizeof(app_id), "%s", id);
	snprintf(source, sizeof(source), "%s", path);
	part = p;
	is_open = 1;
	closing = 0;
	appear = 0.0f;
	zoom = 1.0f;
	cx = cy = 0.5f;
	view = VIEW_PREVIEW;
	const AppItem *it = apps_find(&g_apps, id);
	snprintf(title, sizeof(title), "%s", it ? it->title : id);
	edit_target_size(id, p, &target_w, &target_h);

	if (appdb_livearea(id, &la) != 0)
		memset(&la, 0, sizeof(la));
	gate_known = livearea_gate_origin(la.style, &gate_x, &gate_y) == 0;
	// The measured gate positions assume standard background and gate sizes.
	int gate_w, gate_h;
	edit_target_size(id, ART_GATE, &gate_w, &gate_h);
	can_seamless = p == ART_BG && la.gate_count > 0 && gate_known && target_w == BG_W && target_h == BG_H &&
	               gate_w == GATE_W && gate_h == GATE_H;
	if (!gate_known)
		livearea_gate_origin("a5", &gate_x, &gate_y); // preview position for unknown layouts
	seamless = can_seamless;
}

int editor_is_open(void) {
	return is_open;
}

// --- applying -------------------------------------------------------------------

typedef struct {
	char id[16], source[256];
	ArtPart part;
	Crop crop;
	int seamless, gate_x, gate_y;
	int rc;
	char err[STORE_ERROR_SIZE];
} ApplyJob;

static void apply_work(void *ctx, NetClient *net) {
	(void)net;
	ApplyJob *j = ctx;
	j->rc = edit_make(j->id, j->part, j->source, j->crop, j->seamless, j->gate_x, j->gate_y, j->err);
	if (j->rc != 0)
		return;
	// Remember whether the gate was cut from the background, so a later
	// background without "seamless" doesn't keep a gate that no longer matches.
	char marker[256];
	snprintf(marker, sizeof(marker), STORE_DIR "/%s/custom/gate-from-bg", j->id);
	if (j->part == ART_BG && j->seamless) {
		FILE *f = fopen(marker, "wb");
		if (f)
			fclose(f);
	} else if (j->part == ART_GATE) {
		remove(marker);
	} else if (j->part == ART_BG && remove(marker) == 0) {
		// The current gate was cut from the previous background.
		if (store_clear_part(j->id, ART_GATE, j->err) != 0) {
			j->rc = -1;
			return;
		}
	}
	j->rc = store_apply(j->id, j->err);
}

static void apply_done(void *ctx) {
	ApplyJob *j = ctx;
	app_forget_artwork(j->id);
	if (j->rc != 0) {
		dialog_message("Couldn't apply it", j->err);
	} else {
		app_changed(j->part == ART_ICON, "Applied");
		closing = 1;
		appdetail_open(j->id);
	}
	free(j);
	app_refresh_apps();
}

static void apply(void) {
	if (app_require_unsafe())
		return;
	float sw, sh;
	if (!texture_size(&sw, &sh)) {
		app_toast("The picture is still loading");
		return;
	}
	ApplyJob *j = calloc(1, sizeof(*j));
	if (!j)
		return;
	snprintf(j->id, sizeof(j->id), "%s", app_id);
	snprintf(j->source, sizeof(j->source), "%s", source);
	j->part = part;
	j->crop = current_crop();
	j->seamless = part == ART_BG && seamless;
	j->gate_x = gate_x;
	j->gate_y = gate_y;
	app_run("Applying...", apply_work, apply_done, j);
}

// --- input ----------------------------------------------------------------------

// How many screen pixels one picture-width of crop covers, for touch panning.
static float preview_width(void) {
	return part == ART_ICON ? BUBBLE_BIG : part == ART_BG ? (float)BG_W : (float)GATE_W;
}

void editor_update(const Input *in) {
	if (closing) {
		appear = ui_approach(appear, 0.0f, 20.0f);
		if (appear < 0.02f)
			is_open = 0;
		return;
	}
	appear = ui_approach(appear, 1.0f, 16.0f);
	float dt = ui_dt();
	Crop c = current_crop();

	float px = in->lx, py = in->ly;
	if (in->held & SCE_CTRL_LEFT)
		px = -1.0f;
	if (in->held & SCE_CTRL_RIGHT)
		px = 1.0f;
	if (in->held & SCE_CTRL_UP)
		py = -1.0f;
	if (in->held & SCE_CTRL_DOWN)
		py = 1.0f;
	cx += px * (float)c.w * PAN_SPEED * dt;
	cy += py * (float)c.h * PAN_SPEED * dt;

	float z = -in->ry;
	if (in->held & SCE_CTRL_RTRIGGER)
		z = 1.0f;
	if (in->held & SCE_CTRL_LTRIGGER)
		z = -1.0f;
	zoom *= expf(-z * ZOOM_SPEED * dt);

	if (in->dragging) {
		if (view == VIEW_PREVIEW) {
			// The picture follows the finger.
			cx -= in->drag_dx / preview_width() * (float)c.w;
			cy -= in->drag_dy / (preview_width() * target_h / target_w) * (float)c.h;
		} else {
			cx += in->drag_dx / (SCREEN_W - 2 * MARGIN);
			cy += in->drag_dy / 420.0f;
		}
	}
	clamp_crop();

	if (in->pressed & SCE_CTRL_SQUARE)
		view = view == VIEW_PREVIEW ? VIEW_PICTURE : VIEW_PREVIEW;
	if ((in->pressed & SCE_CTRL_TRIANGLE) && can_seamless)
		seamless = !seamless;
	if (in->pressed & SCE_CTRL_SELECT) {
		zoom = 1.0f;
		cx = cy = 0.5f;
	}
	if (in->pressed & BTN_OK)
		apply();
	if (in->pressed & BTN_BACK)
		closing = 1;
}

// --- drawing --------------------------------------------------------------------

static void draw_current(const char *rel_name, ArtPart p, float x, float y, float w, float h, float r, float a) {
	char path[256];
	if (store_has_custom(app_id, p)) {
		store_custom_path(app_id, p, path, sizeof(path));
	} else {
		char rel[LIVEAREA_NAME_SIZE + 32];
		snprintf(rel, sizeof(rel), "livearea/contents/%s", rel_name);
		store_appmeta_path(app_id, rel, path, sizeof(path));
	}
	ImageRef img = images_file(path);
	if (img.tex)
		ui_image(img.tex, x, y, w, h, r, r, r, r, ui_alpha(0xFFFFFFFF, a * img.alpha));
	else
		ui_rrect(x, y, w, h, r, ui_alpha(C_SURFACE2, a));
}

static void draw_gate_frame(float x, float y, float a) {
	ui_rrect_outline(x - 1.0f, y - 1.0f, GATE_W + 2.0f, GATE_H + 2.0f, 6.0f, 2.0f, ui_alpha(0xFFFFFFFF, a * 0.85f));
}

static void draw_livearea_preview(const vita2d_texture *tex, Crop c, float a) {
	float x = LIVEAREA_X, y = LIVEAREA_Y, gx = x + gate_x, gy = y + gate_y;
	float u0 = (float)c.x, v0 = (float)c.y, u1 = (float)(c.x + c.w), v1 = (float)(c.y + c.h);
	if (part == ART_BG) {
		ui_image_part(tex, x, y, BG_W, BG_H, u0, v0, u1, v1, 8, 8, 0, 0, ui_alpha(0xFFFFFFFF, a));
		if (la.gate_count) {
			ui_shadow(gx, gy, GATE_W, GATE_H, 6.0f, a * 0.5f);
			if (seamless) {
				float g0, h0, g1, h1;
				gate_uv(c, &g0, &h0, &g1, &h1);
				ui_image_part(tex, gx, gy, GATE_W, GATE_H, g0, h0, g1, h1, 5, 5, 5, 5, ui_alpha(0xFFFFFFFF, a));
			} else {
				draw_current(la.gate[0], ART_GATE, gx, gy, GATE_W, GATE_H, 5.0f, a);
			}
			draw_gate_frame(gx, gy, a);
		}
	} else { // ART_GATE
		if (la.bg_count)
			draw_current(la.bg[0], ART_BG, x, y, BG_W, BG_H, 8.0f, a);
		else
			ui_rrect(x, y, BG_W, BG_H, 8.0f, ui_alpha(C_SURFACE, a));
		ui_shadow(gx, gy, GATE_W, GATE_H, 6.0f, a * 0.5f);
		ui_image_part(tex, gx, gy, GATE_W, GATE_H, u0, v0, u1, v1, 5, 5, 5, 5, ui_alpha(0xFFFFFFFF, a));
		draw_gate_frame(gx, gy, a);
	}
	if (!gate_known && !(part == ART_BG && seamless)) {
		char note[96];
		snprintf(note, sizeof(note), "Gate position approximate (layout %s not measured yet)", la.style[0] ? la.style : "?");
		ui_text_right(FONT_SMALL, x + BG_W - 12.0f, y + BG_H - 14.0f, ui_alpha(C_TEXT2, a), note);
	}
}

static void draw_home_preview(const vita2d_texture *tex, Crop c, float a) {
	float u0 = (float)c.x, v0 = (float)c.y, u1 = (float)(c.x + c.w), v1 = (float)(c.y + c.h);
	float r = BUBBLE_BIG * 0.5f;
	ui_shadow(BUBBLE_X, BUBBLE_Y, BUBBLE_BIG, BUBBLE_BIG, r, a);
	ui_image_part(tex, BUBBLE_X, BUBBLE_Y, BUBBLE_BIG, BUBBLE_BIG, u0, v0, u1, v1, r, r, r, r, ui_alpha(0xFFFFFFFF, a));
	ui_text_center(FONT_SMALL, BUBBLE_X + r, BUBBLE_Y + BUBBLE_BIG + 30.0f, ui_alpha(C_TEXT3, a),
	               "The home screen shows icons as circles");

	// A home screen page: this bubble among a few others.
	ui_vgradient(HOME_X, HOME_Y, HOME_W, HOME_H, ui_alpha(RGBA8(40, 60, 96, 255), a), ui_alpha(RGBA8(18, 24, 40, 255), a));
	int others = 0;
	float br = HOME_BUBBLE * 0.5f, gap_x = HOME_W / 4.0f, gap_y = HOME_H / 2.0f;
	for (int slot = 0; slot < 8; slot++) {
		float bx = HOME_X + gap_x * (slot % 4) + (gap_x - HOME_BUBBLE) * 0.5f + (slot / 4 ? gap_x * 0.5f : 0.0f);
		float by = HOME_Y + gap_y * (slot / 4) + (gap_y - HOME_BUBBLE) * 0.5f - 8.0f;
		if (slot / 4 && slot % 4 == 3)
			continue;
		if (slot == 1) {
			ui_image_part(tex, bx, by, HOME_BUBBLE, HOME_BUBBLE, u0, v0, u1, v1, br, br, br, br, ui_alpha(0xFFFFFFFF, a));
			ui_ring(bx + br, by + br, br + 4.0f, 2.0f, 0, 6.2832f, ui_alpha(0xFFFFFFFF, a * 0.8f));
			continue;
		}
		// Neighbours: other installed apps' real icons.
		const AppItem *other = NULL;
		while (others < g_apps.count && !other) {
			const AppItem *it = &g_apps.items[others++];
			if (!it->system && strcmp(it->id, app_id) != 0)
				other = it;
		}
		ImageRef img = {0};
		if (other) {
			char path[256];
			store_appmeta_path(other->id, "icon0.png", path, sizeof(path));
			img = images_file(path);
		}
		if (img.tex)
			ui_image(img.tex, bx, by, HOME_BUBBLE, HOME_BUBBLE, br, br, br, br, ui_alpha(0xFFFFFFFF, a * img.alpha * 0.9f));
		else
			ui_rrect(bx, by, HOME_BUBBLE, HOME_BUBBLE, br, ui_alpha(C_SURFACE3, a));
	}
}

static void draw_picture_view(const vita2d_texture *tex, Crop c, float a) {
	float sw = (float)vita2d_texture_get_width(tex), sh = (float)vita2d_texture_get_height(tex);
	float aw = SCREEN_W - 2 * MARGIN, ah = 420.0f, top = 72.0f;
	float s = fminf(aw / sw, ah / sh), w = sw * s, h = sh * s;
	float x = (SCREEN_W - w) * 0.5f, y = top + (ah - h) * 0.5f;
	ui_image(tex, x, y, w, h, 0, 0, 0, 0, ui_alpha(RGBA8(90, 90, 90, 255), a));
	float fx = x + (float)c.x * w, fy = y + (float)c.y * h, fw = (float)c.w * w, fh = (float)c.h * h;
	ui_image_part(tex, fx, fy, fw, fh, (float)c.x, (float)c.y, (float)(c.x + c.w), (float)(c.y + c.h), 0, 0, 0, 0,
	              ui_alpha(0xFFFFFFFF, a));
	ui_rrect_outline(fx, fy, fw, fh, 2.0f, 2.0f, ui_alpha(C_ACCENT, a));
	if (part == ART_BG && seamless)
		ui_rrect_outline(fx + fw * gate_x / BG_W, fy + fh * gate_y / BG_H, fw * GATE_W / BG_W, fh * GATE_H / BG_H, 2.0f,
		                 1.5f, ui_alpha(0xFFFFFFFF, a * 0.9f));
	char size[64];
	snprintf(size, sizeof(size), "Output %dx%d \xC2\xB7 256 colours", target_w, target_h);
	ui_text_center(FONT_SMALL, SCREEN_W * 0.5f, top + ah + 22.0f, ui_alpha(C_TEXT3, a), size);
}

void editor_draw(void) {
	if (!is_open)
		return;
	float a = appear;
	ui_rect(0, 0, SCREEN_W, SCREEN_H, ui_alpha(RGBA8(4, 4, 4, 255), a));

	ImageRef img = images_file(source);
	Crop c = current_crop();
	if (img.tex) {
		if (view == VIEW_PICTURE)
			draw_picture_view(img.tex, c, a * img.alpha);
		else if (part == ART_ICON)
			draw_home_preview(img.tex, c, a * img.alpha);
		else
			draw_livearea_preview(img.tex, c, a * img.alpha);
	} else if (img.failed) {
		ui_text_center(FONT_TITLE, SCREEN_W * 0.5f, SCREEN_H * 0.5f, ui_alpha(C_TEXT, a), "Couldn't open this picture");
	} else {
		ui_spinner(SCREEN_W * 0.5f, SCREEN_H * 0.5f, 14.0f, ui_alpha(C_ACCENT, a));
	}

	// Top line: what is being edited (fits in the strip above the LiveArea).
	static const char *const names[] = {"Bubble icon", "LiveArea background", "Start gate"};
	char heading[200], fitted[200];
	snprintf(heading, sizeof(heading), "%s \xC2\xB7 %s", names[part], title);
	ui_text_fit(FONT_BOLD, heading, 560, fitted, sizeof(fitted));
	ui_text(FONT_BOLD, LIVEAREA_X, 30.0f, ui_alpha(C_TEXT, a), fitted);
	if (part == ART_BG && la.gate_count) {
		const char *label = !can_seamless ? "Seamless gate not available for this layout yet"
		                    : seamless    ? "Seamless gate: on"
		                                  : "Seamless gate: off";
		ui_text_right(FONT_SMALL, SCREEN_W - LIVEAREA_X, 29.0f, ui_alpha(seamless ? C_SUCCESS : C_TEXT3, a), label);
	}

	// Footer over a scrim, since the LiveArea reaches the bottom of the screen.
	ui_vgradient(0, FOOTER_Y - 18.0f, SCREEN_W, SCREEN_H - FOOTER_Y + 18.0f, ui_alpha(RGBA8(0, 0, 0, 0), a),
	             ui_alpha(RGBA8(0, 0, 0, 220), a));
	Hint hints[5];
	int n = 0;
	hints[n++] = (Hint){ui_ok_glyph(), "Apply"};
	hints[n++] = (Hint){ui_back_glyph(), "Back"};
	hints[n++] = (Hint){GLYPH_SQUARE, view == VIEW_PREVIEW ? "Whole picture" : "Preview"};
	if (can_seamless)
		hints[n++] = (Hint){GLYPH_TRIANGLE, "Seamless gate"};
	hints[n++] = (Hint){GLYPH_R, "Zoom"};
	app_footer(hints, n, "Stick or drag: move \xC2\xB7 L: zoom out \xC2\xB7 SELECT: reset");
}
