// Drawing primitives, typography and the colour palette.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vita2d.h>

#define SCREEN_W 960
#define SCREEN_H 544

// Palette
#define C_BG RGBA8(9, 8, 7, 255)
#define C_SURFACE RGBA8(24, 22, 20, 255)
#define C_SURFACE2 RGBA8(35, 32, 29, 255)
#define C_SURFACE3 RGBA8(52, 47, 43, 255)
#define C_STROKE RGBA8(255, 244, 228, 20)
#define C_TEXT RGBA8(246, 238, 222, 255)
#define C_TEXT2 RGBA8(178, 166, 150, 255)
#define C_TEXT3 RGBA8(122, 112, 102, 255)
#define C_ACCENT RGBA8(214, 46, 54, 255)
#define C_ACCENT2 RGBA8(150, 34, 42, 255)
#define C_ON_ACCENT RGBA8(255, 248, 238, 255)
#define C_SUCCESS RGBA8(74, 222, 128, 255)
#define C_WARNING RGBA8(251, 191, 36, 255)
#define C_DANGER RGBA8(255, 112, 96, 255)
#define C_SCRIM RGBA8(6, 5, 4, 190)

typedef enum {
	FONT_SMALL,   // 13 px
	FONT_BODY,    // 16 px
	FONT_BOLD,    // 16 px semibold
	FONT_TITLE,   // 19 px semibold
	FONT_HEADING, // 26 px semibold
	FONT_COUNT
} Font;

typedef enum { GLYPH_CROSS, GLYPH_CIRCLE, GLYPH_TRIANGLE, GLYPH_SQUARE, GLYPH_L, GLYPH_R, GLYPH_START, GLYPH_SELECT } Glyph;

int ui_init(void);
void ui_shutdown(void);
void ui_begin_frame(void);
float ui_dt(void);   // seconds since the previous frame (clamped)
float ui_time(void); // seconds since start

uint32_t ui_alpha(uint32_t color, float alpha); // multiplies the colour's alpha
uint32_t ui_mix(uint32_t a, uint32_t b, float t);
// Frame-rate independent exponential approach of current towards target.
float ui_approach(float current, float target, float speed);

void ui_rect(float x, float y, float w, float h, uint32_t color);
void ui_rrect(float x, float y, float w, float h, float r, uint32_t color);
void ui_rrect_outline(float x, float y, float w, float h, float r, float thickness, uint32_t color);
void ui_vgradient(float x, float y, float w, float h, uint32_t top, uint32_t bottom);
void ui_line(float x0, float y0, float x1, float y1, float thickness, uint32_t color);
void ui_ring(float cx, float cy, float r, float thickness, float from, float to, uint32_t color);
void ui_spinner(float cx, float cy, float r, uint32_t color);
void ui_progress(float x, float y, float w, float h, float progress, uint32_t track, uint32_t fill);
void ui_shadow(float x, float y, float w, float h, float r, float strength);

// Draws a texture stretched into the box with individually rounded corners
// (top-left, top-right, bottom-right, bottom-left).
void ui_image(const vita2d_texture *tex, float x, float y, float w, float h, float r_tl, float r_tr,
              float r_br, float r_bl, uint32_t tint);

// Text. y is the baseline. Functions return the drawn width.
int ui_font_size(Font font);
int ui_text(Font font, float x, float y, uint32_t color, const char *text);
int ui_text_width(Font font, const char *text);
int ui_text_right(Font font, float right, float y, uint32_t color, const char *text);
int ui_text_center(Font font, float cx, float y, uint32_t color, const char *text);
// Copies text into out, shortened with an ellipsis to fit max_width.
void ui_text_fit(Font font, const char *text, int max_width, char *out, size_t size);
// Word-wraps text; returns the number of lines drawn.
int ui_text_wrap(Font font, float x, float y, int max_width, int line_height, int max_lines,
                 uint32_t color, const char *text);
int ui_text_wrap_lines(Font font, int max_width, const char *text);

// Button prompts. ui_glyph draws one centred at cx,cy and returns its width.
int ui_glyph(Glyph glyph, float cx, float cy, float alpha);
int ui_glyph_width(Glyph glyph);
// Draws "glyph label" right-aligned at `right`; returns the left edge.
float ui_hint(float right, float cy, Glyph glyph, const char *label);
Glyph ui_ok_glyph(void);
Glyph ui_back_glyph(void);

// Small rounded label, e.g. "NEW". Returns its width.
int ui_badge(float x, float y, const char *label, uint32_t background, uint32_t foreground);
int ui_badge_width(const char *label);
