#include "ui.h"

#include "fs.h"
#include "input.h"

#include <math.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265f
#define CORNER_SEGMENTS 6
#define MAX_PERIMETER (4 * (CORNER_SEGMENTS + 1))

// vita2d's glyph atlas is keyed by glyph only, so a font object used at two
// sizes would rescale the first bitmap. One object per size keeps text crisp.
static const struct {
	int bold;
	int size;
} font_specs[FONT_COUNT] = {
	[FONT_SMALL] = {0, 13}, [FONT_BODY] = {0, 16}, [FONT_BOLD] = {1, 16},
	[FONT_TITLE] = {1, 19}, [FONT_HEADING] = {1, 26},
};

static vita2d_font *fonts[FONT_COUNT];
static char *font_data[2];
static size_t font_size[2];
static vita2d_pgf *fallback; // system font with CJK coverage
static unsigned long long start_us, last_us;
static float frame_dt;

int ui_init(void) {
	static const char *const paths[2] = {"app0:assets/Inter-Regular.ttf", "app0:assets/Inter-SemiBold.ttf"};
	for (int i = 0; i < 2; i++)
		if (fs_read_file(paths[i], &font_data[i], &font_size[i]) != 0)
			return -1;
	for (int i = 0; i < FONT_COUNT; i++) {
		int b = font_specs[i].bold;
		fonts[i] = vita2d_load_font_mem(font_data[b], (unsigned int)font_size[b]);
		if (!fonts[i])
			return -1;
	}
	fallback = vita2d_load_default_pgf();
	start_us = last_us = sceKernelGetProcessTimeWide();
	return 0;
}

void ui_shutdown(void) {
	for (int i = 0; i < FONT_COUNT; i++)
		if (fonts[i])
			vita2d_free_font(fonts[i]);
	if (fallback)
		vita2d_free_pgf(fallback);
	free(font_data[0]);
	free(font_data[1]);
}

void ui_begin_frame(void) {
	unsigned long long now = sceKernelGetProcessTimeWide();
	frame_dt = (float)(now - last_us) / 1000000.0f;
	if (frame_dt > 0.1f)
		frame_dt = 0.1f;
	last_us = now;
}

float ui_dt(void) {
	return frame_dt;
}

float ui_time(void) {
	return (float)(sceKernelGetProcessTimeWide() - start_us) / 1000000.0f;
}

uint32_t ui_alpha(uint32_t color, float alpha) {
	if (alpha <= 0.0f)
		return color & 0x00FFFFFF;
	if (alpha >= 1.0f)
		return color;
	uint32_t a = (uint32_t)((float)(color >> 24) * alpha);
	return (color & 0x00FFFFFF) | (a << 24);
}

uint32_t ui_mix(uint32_t a, uint32_t b, float t) {
	uint32_t out = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		float ca = (float)((a >> shift) & 0xFF), cb = (float)((b >> shift) & 0xFF);
		out |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << shift;
	}
	return out;
}

float ui_approach(float current, float target, float speed) {
	float k = 1.0f - expf(-speed * frame_dt);
	float next = current + (target - current) * k;
	return fabsf(target - next) < 0.05f ? target : next;
}

// --- geometry ---------------------------------------------------------------

static int perimeter(float x, float y, float w, float h, const float radius[4], float *px, float *py) {
	float lim = (w < h ? w : h) * 0.5f;
	const float cx[4] = {x, x + w, x + w, x};
	const float cy[4] = {y, y, y + h, y + h};
	const float sx[4] = {1, -1, -1, 1}, sy[4] = {1, 1, -1, -1};
	int n = 0;
	for (int c = 0; c < 4; c++) {
		float r = radius[c] > lim ? lim : radius[c] < 0 ? 0 : radius[c];
		float ox = cx[c] + sx[c] * r, oy = cy[c] + sy[c] * r;
		float start = PI + (float)c * PI * 0.5f;
		for (int s = 0; s <= CORNER_SEGMENTS; s++) {
			float a = start + (PI * 0.5f) * (float)s / CORNER_SEGMENTS;
			px[n] = ox + cosf(a) * r;
			py[n] = oy + sinf(a) * r;
			n++;
		}
	}
	return n;
}

static vita2d_color_vertex *color_vertices(int count) {
	return vita2d_pool_memalign((unsigned)(count * (int)sizeof(vita2d_color_vertex)), sizeof(vita2d_color_vertex));
}

void ui_rect(float x, float y, float w, float h, uint32_t color) {
	vita2d_draw_rectangle(x, y, w, h, color);
}

void ui_rrect(float x, float y, float w, float h, float r, uint32_t color) {
	if (r <= 0.5f) {
		vita2d_draw_rectangle(x, y, w, h, color);
		return;
	}
	float px[MAX_PERIMETER], py[MAX_PERIMETER];
	const float radius[4] = {r, r, r, r};
	int n = perimeter(x, y, w, h, radius, px, py);
	vita2d_color_vertex *v = color_vertices(n + 2);
	if (!v)
		return;
	v[0] = (vita2d_color_vertex){x + w * 0.5f, y + h * 0.5f, 0.5f, color};
	for (int i = 0; i <= n; i++)
		v[i + 1] = (vita2d_color_vertex){px[i % n], py[i % n], 0.5f, color};
	vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, (size_t)n + 2);
}

void ui_rrect_outline(float x, float y, float w, float h, float r, float t, uint32_t color) {
	float ox[MAX_PERIMETER], oy[MAX_PERIMETER], ix[MAX_PERIMETER], iy[MAX_PERIMETER];
	const float outer[4] = {r, r, r, r};
	const float inner[4] = {r - t, r - t, r - t, r - t};
	int n = perimeter(x, y, w, h, outer, ox, oy);
	perimeter(x + t, y + t, w - 2 * t, h - 2 * t, inner, ix, iy);
	vita2d_color_vertex *v = color_vertices(2 * n + 2);
	if (!v)
		return;
	for (int i = 0; i <= n; i++) {
		v[2 * i] = (vita2d_color_vertex){ox[i % n], oy[i % n], 0.5f, color};
		v[2 * i + 1] = (vita2d_color_vertex){ix[i % n], iy[i % n], 0.5f, color};
	}
	vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, (size_t)(2 * n + 2));
}

void ui_vgradient(float x, float y, float w, float h, uint32_t top, uint32_t bottom) {
	vita2d_color_vertex *v = color_vertices(4);
	if (!v)
		return;
	v[0] = (vita2d_color_vertex){x, y, 0.5f, top};
	v[1] = (vita2d_color_vertex){x + w, y, 0.5f, top};
	v[2] = (vita2d_color_vertex){x, y + h, 0.5f, bottom};
	v[3] = (vita2d_color_vertex){x + w, y + h, 0.5f, bottom};
	vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4);
}

void ui_line(float x0, float y0, float x1, float y1, float t, uint32_t color) {
	float dx = x1 - x0, dy = y1 - y0;
	float len = sqrtf(dx * dx + dy * dy);
	if (len < 0.001f)
		return;
	float nx = -dy / len * t * 0.5f, ny = dx / len * t * 0.5f;
	vita2d_color_vertex *v = color_vertices(4);
	if (!v)
		return;
	v[0] = (vita2d_color_vertex){x0 + nx, y0 + ny, 0.5f, color};
	v[1] = (vita2d_color_vertex){x0 - nx, y0 - ny, 0.5f, color};
	v[2] = (vita2d_color_vertex){x1 + nx, y1 + ny, 0.5f, color};
	v[3] = (vita2d_color_vertex){x1 - nx, y1 - ny, 0.5f, color};
	vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4);
}

void ui_ring(float cx, float cy, float r, float t, float from, float to, uint32_t color) {
	enum { SEG = 32 };
	vita2d_color_vertex *v = color_vertices(2 * (SEG + 1));
	if (!v)
		return;
	for (int i = 0; i <= SEG; i++) {
		float a = from + (to - from) * (float)i / SEG;
		float c = cosf(a), s = sinf(a);
		v[2 * i] = (vita2d_color_vertex){cx + c * r, cy + s * r, 0.5f, color};
		v[2 * i + 1] = (vita2d_color_vertex){cx + c * (r - t), cy + s * (r - t), 0.5f, color};
	}
	vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 2 * (SEG + 1));
}

void ui_spinner(float cx, float cy, float r, uint32_t color) {
	float t = ui_time();
	float start = t * 5.5f;
	float sweep = PI * (0.9f + 0.5f * sinf(t * 2.2f));
	ui_ring(cx, cy, r, r * 0.28f, 0, 2 * PI, ui_alpha(color, 0.18f));
	ui_ring(cx, cy, r, r * 0.28f, start, start + sweep, color);
}

void ui_progress(float x, float y, float w, float h, float p, uint32_t track, uint32_t fill) {
	p = p < 0 ? 0 : p > 1 ? 1 : p;
	ui_rrect(x, y, w, h, h * 0.5f, track);
	if (p > 0.0f)
		ui_rrect(x, y, w * p < h ? h : w * p, h, h * 0.5f, fill);
}

void ui_shadow(float x, float y, float w, float h, float r, float strength) {
	for (int i = 3; i >= 1; i--) {
		float g = (float)i * 3.0f;
		ui_rrect(x - g, y - g + 4, w + 2 * g, h + 2 * g, r + g, RGBA8(0, 0, 0, (int)(28 * strength)));
	}
}

void ui_image(const vita2d_texture *tex, float x, float y, float w, float h, float r_tl, float r_tr,
              float r_br, float r_bl, uint32_t tint) {
	float px[MAX_PERIMETER], py[MAX_PERIMETER];
	const float radius[4] = {r_tl, r_tr, r_br, r_bl};
	int n = perimeter(x, y, w, h, radius, px, py);
	vita2d_texture_vertex *v = vita2d_pool_memalign((unsigned)((n + 2) * (int)sizeof(vita2d_texture_vertex)),
	                                                sizeof(vita2d_texture_vertex));
	if (!v)
		return;
	v[0] = (vita2d_texture_vertex){x + w * 0.5f, y + h * 0.5f, 0.5f, 0.5f, 0.5f};
	for (int i = 0; i <= n; i++) {
		float vx = px[i % n], vy = py[i % n];
		v[i + 1] = (vita2d_texture_vertex){vx, vy, 0.5f, (vx - x) / w, (vy - y) / h};
	}
	vita2d_draw_array_textured(tex, SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, (size_t)n + 2, tint);
}

void ui_image_part(const vita2d_texture *tex, float x, float y, float w, float h, float u0, float v0, float u1,
                   float v1, float r_tl, float r_tr, float r_br, float r_bl, uint32_t tint) {
	float px[MAX_PERIMETER], py[MAX_PERIMETER];
	const float radius[4] = {r_tl, r_tr, r_br, r_bl};
	int n = perimeter(x, y, w, h, radius, px, py);
	vita2d_texture_vertex *v = vita2d_pool_memalign((unsigned)((n + 2) * (int)sizeof(vita2d_texture_vertex)),
	                                                sizeof(vita2d_texture_vertex));
	if (!v)
		return;
	float du = u1 - u0, dv = v1 - v0;
	v[0] = (vita2d_texture_vertex){x + w * 0.5f, y + h * 0.5f, 0.5f, u0 + du * 0.5f, v0 + dv * 0.5f};
	for (int i = 0; i <= n; i++) {
		float vx = px[i % n], vy = py[i % n];
		v[i + 1] = (vita2d_texture_vertex){vx, vy, 0.5f, u0 + du * (vx - x) / w, v0 + dv * (vy - y) / h};
	}
	vita2d_draw_array_textured(tex, SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, (size_t)n + 2, tint);
}

// --- text -------------------------------------------------------------------

static unsigned decode_utf8(const char **p) {
	const unsigned char *s = (const unsigned char *)*p;
	unsigned c = s[0];
	int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
	if (extra)
		c &= 0x3F >> extra;
	(*p)++;
	for (int i = 0; i < extra && (**p & 0xC0) == 0x80; i++, (*p)++)
		c = (c << 6) | ((unsigned char)**p & 0x3F);
	return c;
}

// Inter covers Latin, Greek and Cyrillic; CJK and symbol-heavy titles go to
// the system font instead of rendering as missing glyphs.
static int needs_fallback(const char *text) {
	for (const char *p = text; *p;) {
		unsigned c = decode_utf8(&p);
		if (c >= 0x2460 && fallback)
			return 1;
	}
	return 0;
}

static float fallback_scale(Font font) {
	return (float)font_specs[font].size / 18.0f;
}

int ui_font_size(Font font) {
	return font_specs[font].size;
}

int ui_text_width(Font font, const char *text) {
	if (!text[0])
		return 0;
	if (needs_fallback(text))
		return vita2d_pgf_text_width(fallback, fallback_scale(font), text);
	return vita2d_font_text_width(fonts[font], (unsigned)font_specs[font].size, text);
}

int ui_text(Font font, float x, float y, uint32_t color, const char *text) {
	if (!text[0])
		return 0;
	if (needs_fallback(text)) {
		float s = fallback_scale(font);
		vita2d_pgf_draw_text(fallback, (int)x, (int)y, color, s, text);
		return vita2d_pgf_text_width(fallback, s, text);
	}
	return vita2d_font_draw_text(fonts[font], (int)x, (int)y, color, (unsigned)font_specs[font].size, text);
}

int ui_text_right(Font font, float right, float y, uint32_t color, const char *text) {
	int w = ui_text_width(font, text);
	ui_text(font, right - (float)w, y, color, text);
	return w;
}

int ui_text_center(Font font, float cx, float y, uint32_t color, const char *text) {
	int w = ui_text_width(font, text);
	ui_text(font, cx - (float)w * 0.5f, y, color, text);
	return w;
}

static size_t prev_char(const char *s, size_t len) {
	while (len > 0 && ((unsigned char)s[--len] & 0xC0) == 0x80) {
	}
	return len;
}

void ui_text_fit(Font font, const char *text, int max_width, char *out, size_t size) {
	snprintf(out, size, "%s", text);
	if (ui_text_width(font, out) <= max_width)
		return;
	static const char ellipsis[] = "\xE2\x80\xA6";
	size_t len = strlen(out);
	while (len > 0) {
		len = prev_char(out, len);
		while (len > 0 && out[len - 1] == ' ')
			len--;
		if (len + sizeof(ellipsis) > size)
			continue;
		memcpy(out + len, ellipsis, sizeof(ellipsis));
		if (ui_text_width(font, out) <= max_width)
			return;
	}
	snprintf(out, size, "%s", ellipsis);
}

// Calls emit(line) for each wrapped line; returns the line count.
static int wrap(Font font, int max_width, int max_lines, const char *text,
                void (*emit)(int index, const char *line, void *user), void *user) {
	char line[512];
	int lines = 0;
	const char *p = text;
	while (*p && (max_lines <= 0 || lines < max_lines)) {
		while (*p == ' ')
			p++;
		// Greedily add words while the line still fits.
		const char *end = p;
		for (const char *q = p; *q && *q != '\n';) {
			const char *word_end = q;
			while (*word_end && *word_end != ' ' && *word_end != '\n')
				word_end++;
			size_t candidate = (size_t)(word_end - p);
			if (candidate >= sizeof(line))
				break;
			memcpy(line, p, candidate);
			line[candidate] = '\0';
			int too_wide = ui_text_width(font, line) > max_width;
			if (too_wide && end != p)
				break;
			end = word_end; // a single over-long word is kept and ellipsised below
			if (too_wide)
				break;
			for (q = word_end; *q == ' ';)
				q++;
		}
		size_t len = (size_t)(end - p);
		if (len == 0 && *p && *p != '\n') { // one enormous word: hard cut
			len = strlen(p);
			if (len > sizeof(line) - 1)
				len = sizeof(line) - 1;
		}
		memcpy(line, p, len);
		line[len] = '\0';
		p += len;
		while (*p == ' ')
			p++;
		if (*p == '\n')
			p++;
		int last = max_lines > 0 && lines == max_lines - 1 && *p;
		if (last) { // ellipsise the final visible line
			char rest[512];
			snprintf(rest, sizeof(rest), "%s %s", line, p);
			ui_text_fit(font, rest, max_width, line, sizeof(line));
		} else if (ui_text_width(font, line) > max_width) {
			char fitted[512];
			ui_text_fit(font, line, max_width, fitted, sizeof(fitted));
			snprintf(line, sizeof(line), "%s", fitted);
		}
		if (emit)
			emit(lines, line, user);
		lines++;
	}
	return lines;
}

typedef struct {
	Font font;
	float x, y;
	int line_height;
	uint32_t color;
} WrapDraw;

static void emit_line(int index, const char *line, void *user) {
	WrapDraw *d = user;
	ui_text(d->font, d->x, d->y + (float)(index * d->line_height), d->color, line);
}

int ui_text_wrap(Font font, float x, float y, int max_width, int line_height, int max_lines,
                 uint32_t color, const char *text) {
	WrapDraw d = {font, x, y, line_height, color};
	return wrap(font, max_width, max_lines, text, emit_line, &d);
}

int ui_text_wrap_lines(Font font, int max_width, const char *text) {
	return wrap(font, max_width, 0, text, NULL, NULL);
}

// --- glyphs -------------------------------------------------------------------

Glyph ui_ok_glyph(void) {
	return BTN_OK == SCE_CTRL_CIRCLE ? GLYPH_CIRCLE : GLYPH_CROSS;
}

Glyph ui_back_glyph(void) {
	return BTN_BACK == SCE_CTRL_CROSS ? GLYPH_CROSS : GLYPH_CIRCLE;
}

static int pill_glyph(const char *label, float cx, float cy, float alpha) {
	int tw = ui_text_width(FONT_SMALL, label);
	float w = (float)tw + 12.0f, h = 18.0f;
	ui_rrect(cx - w * 0.5f, cy - h * 0.5f, w, h, 5.0f, ui_alpha(C_SURFACE3, alpha));
	ui_text(FONT_SMALL, cx - (float)tw * 0.5f, cy + 4.5f, ui_alpha(C_TEXT, alpha), label);
	return (int)w;
}

int ui_glyph(Glyph glyph, float cx, float cy, float alpha) {
	const float r = 9.5f;
	switch (glyph) {
	case GLYPH_L: return pill_glyph("L", cx, cy, alpha);
	case GLYPH_R: return pill_glyph("R", cx, cy, alpha);
	case GLYPH_START: return pill_glyph("START", cx, cy, alpha);
	case GLYPH_SELECT: return pill_glyph("SELECT", cx, cy, alpha);
	default: break;
	}
	vita2d_draw_fill_circle(cx, cy, r, ui_alpha(C_SURFACE3, alpha));
	float s = 4.2f, t = 1.8f;
	switch (glyph) {
	case GLYPH_CROSS: {
		uint32_t c = ui_alpha(RGBA8(125, 167, 236, 255), alpha);
		ui_line(cx - s, cy - s, cx + s, cy + s, t, c);
		ui_line(cx - s, cy + s, cx + s, cy - s, t, c);
		break;
	}
	case GLYPH_CIRCLE:
		ui_ring(cx, cy, s + 0.8f, t, 0, 2 * PI, ui_alpha(RGBA8(240, 110, 110, 255), alpha));
		break;
	case GLYPH_TRIANGLE: {
		uint32_t c = ui_alpha(RGBA8(64, 206, 168, 255), alpha);
		float top = cy - s - 0.5f, bottom = cy + s * 0.75f;
		ui_line(cx, top, cx + s + 0.6f, bottom, t, c);
		ui_line(cx + s + 0.6f, bottom, cx - s - 0.6f, bottom, t, c);
		ui_line(cx - s - 0.6f, bottom, cx, top, t, c);
		break;
	}
	case GLYPH_SQUARE:
		ui_rrect_outline(cx - s, cy - s, 2 * s, 2 * s, 1.0f, t, ui_alpha(RGBA8(226, 140, 200, 255), alpha));
		break;
	default:
		break;
	}
	return (int)(r * 2);
}

int ui_glyph_width(Glyph glyph) {
	switch (glyph) {
	case GLYPH_L:
	case GLYPH_R: return ui_text_width(FONT_SMALL, "L") + 12;
	case GLYPH_START: return ui_text_width(FONT_SMALL, "START") + 12;
	case GLYPH_SELECT: return ui_text_width(FONT_SMALL, "SELECT") + 12;
	default: return 19;
	}
}

float ui_hint(float right, float cy, Glyph glyph, const char *label) {
	int lw = ui_text_width(FONT_SMALL, label);
	float x = right - (float)lw;
	ui_text(FONT_SMALL, x, cy + 4.5f, C_TEXT2, label);
	float gw = (float)ui_glyph_width(glyph);
	x -= 6.0f + gw;
	ui_glyph(glyph, x + gw * 0.5f, cy, 1.0f);
	return x - 18.0f;
}

int ui_badge_width(const char *label) {
	return ui_text_width(FONT_SMALL, label) + 14;
}

int ui_badge(float x, float y, const char *label, uint32_t background, uint32_t foreground) {
	int w = ui_badge_width(label);
	ui_rrect(x, y, (float)w, 20.0f, 6.0f, background);
	ui_text(FONT_SMALL, x + 7.0f, y + 14.5f, foreground, label);
	return w;
}
