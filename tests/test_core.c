// Host-side tests for the LiveArea template parser, param.sfo reading and the
// colour quantizer.
#include "../src/livearea.h"
#include "../src/quant.h"
#include "../src/sfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
			failures++;                                                                            \
		}                                                                                          \
	} while (0)

// The template vitasdk samples (and most homebrew) ship.
static const char *HOMEBREW =
	"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
	"<livearea style=\"a1\" format-ver=\"01.00\" content-rev=\"1\">\n"
	"  <livearea-background>\n    <image>bg.png</image>\n  </livearea-background>\n"
	"  <gate>\n    <startup-image>startup.png</startup-image>\n  </gate>\n"
	"</livearea>\n";

// A retail-style template: language variants as mixed content, subfolders
// with backslashes, single quotes, a BOM, padding and live frames.
static const char *RETAIL =
	"\xEF\xBB\xBF<?xml version='1.0' encoding='utf-8'?>\n"
	"<livearea style='a4' format-ver='01.00'>"
	"<livearea-background><image default=\"on\">backgrounds\\bg0.png<cntry lang=\"en\">all</cntry></image>"
	"<image>bg0.png<lang>ja</lang><lang>en</lang></image></livearea-background>"
	"<gate><startup-image>config_textures/bggate.png </startup-image>"
	"<startup-image>gates\\startup_spanish.PNG<cntry lang=\"es\">all</cntry></startup-image></gate>"
	"<frame id='frame1'><liveitem><image>frame1.png</image></liveitem></frame>"
	"<frame id='frame2'><liveitem><text><str>Hi</str></text></liveitem></frame>"
	"</livearea>";

// File names that try to leave livearea/contents are dropped.
static const char *HOSTILE =
	"<livearea style=\"a1\"><livearea-background><image>../../icon0.png</image><image>/abs.png</image>"
	"<image>ur0:x.png</image><image>ok.png</image></livearea-background>"
	"<gate><startup-image>a/../../b.png</startup-image></gate></livearea>";

static void test_livearea(void) {
	LiveArea la;
	CHECK(livearea_parse(HOMEBREW, &la) == 0);
	CHECK(strcmp(la.style, "a1") == 0);
	CHECK(la.bg_count == 1 && strcmp(la.bg[0], "bg.png") == 0);
	CHECK(la.gate_count == 1 && strcmp(la.gate[0], "startup.png") == 0);
	CHECK(la.frame_count == 0);

	CHECK(livearea_parse(RETAIL, &la) == 0);
	CHECK(strcmp(la.style, "a4") == 0);
	CHECK(la.bg_count == 2 && strcmp(la.bg[0], "backgrounds/bg0.png") == 0 && strcmp(la.bg[1], "bg0.png") == 0);
	CHECK(la.gate_count == 2 && strcmp(la.gate[0], "config_textures/bggate.png") == 0 &&
	      strcmp(la.gate[1], "gates/startup_spanish.PNG") == 0);
	CHECK(la.frame_count == 2);

	CHECK(livearea_parse(HOSTILE, &la) == 0);
	CHECK(la.bg_count == 1 && strcmp(la.bg[0], "ok.png") == 0);
	CHECK(la.gate_count == 0);

	CHECK(livearea_parse("<notlivearea/>", &la) != 0);
	CHECK(livearea_parse("", &la) != 0);

	// Names from app.db keep a leading space (" 23.png" is a real file name).
	char name[LIVEAREA_NAME_SIZE];
	CHECK(livearea_clean_name(" 23.png", 7, 0, name, sizeof(name)) == 0 && strcmp(name, " 23.png") == 0);
	CHECK(livearea_clean_name(" bg.png ", 8, 1, name, sizeof(name)) == 0 && strcmp(name, "bg.png") == 0);
	CHECK(livearea_clean_name("gates\\a.png", 11, 0, name, sizeof(name)) == 0 && strcmp(name, "gates/a.png") == 0);
	CHECK(livearea_clean_name("..\\a.png", 8, 0, name, sizeof(name)) != 0);
	CHECK(livearea_clean_name("", 0, 0, name, sizeof(name)) != 0);

	int x, y;
	CHECK(livearea_gate_origin("a5", &x, &y) == 0 && x == 520 && y == 105);
	CHECK(livearea_gate_origin("a1", &x, &y) == 0 && x == 280 && y == 139);
	CHECK(livearea_gate_origin("psmobile", &x, &y) == 0 && x == 520 && y == 155);
	CHECK(livearea_gate_origin("ad0", &x, &y) != 0);
}

static void put32(unsigned char *p, unsigned v) {
	p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8), p[2] = (unsigned char)(v >> 16), p[3] = (unsigned char)(v >> 24);
}

static void put16(unsigned char *p, unsigned v) {
	p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8);
}

static void test_sfo(void) {
	// Two entries: APP_VER and TITLE.
	unsigned char sfo[128];
	memset(sfo, 0, sizeof(sfo));
	memcpy(sfo, "\0PSF", 4);
	put32(sfo + 4, 0x101);
	unsigned keys = 20 + 2 * 16, values = keys + 16;
	put32(sfo + 8, keys);
	put32(sfo + 12, values);
	put32(sfo + 16, 2);
	memcpy(sfo + keys, "APP_VER\0TITLE\0", 14);
	put16(sfo + 20, 0), put16(sfo + 22, 0x0204), put32(sfo + 24, 6), put32(sfo + 28, 8), put32(sfo + 32, 0);
	put16(sfo + 36, 8), put16(sfo + 38, 0x0204), put32(sfo + 40, 7), put32(sfo + 44, 16), put32(sfo + 48, 8);
	memcpy(sfo + values, "01.00\0\0\0Imago\0\0", 15);
	char out[32];
	CHECK(sfo_get_string(sfo, sizeof(sfo), "TITLE", out, sizeof(out)) == 0 && strcmp(out, "Imago") == 0);
	CHECK(sfo_get_string(sfo, sizeof(sfo), "APP_VER", out, sizeof(out)) == 0 && strcmp(out, "01.00") == 0);
	CHECK(sfo_get_string(sfo, sizeof(sfo), "STITLE", out, sizeof(out)) != 0 && out[0] == '\0');
	CHECK(sfo_get_string(sfo, 30, "TITLE", out, sizeof(out)) != 0); // truncated
	char small[4];
	CHECK(sfo_get_string(sfo, sizeof(sfo), "TITLE", small, sizeof(small)) == 0 && strcmp(small, "Ima") == 0);
	sfo[0] = 'X';
	CHECK(sfo_get_string(sfo, sizeof(sfo), "TITLE", out, sizeof(out)) != 0);
}

static void test_quant(void) {
	enum { W = 840, H = 500 };
	uint8_t *rgba = malloc(W * H * 4), *idx = malloc(W * H), palette[256 * 4];

	// A smooth two-axis gradient: far more than 256 colours.
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			uint8_t *p = rgba + (y * W + x) * 4;
			p[0] = (uint8_t)(x * 255 / (W - 1)), p[1] = (uint8_t)(y * 255 / (H - 1)), p[2] = (uint8_t)((x + y) % 256), p[3] = 255;
		}
	int colors = quant_rgba(rgba, W, H, 1, idx, palette);
	CHECK(colors > 200 && colors <= 256);
	double err = 0;
	for (int i = 0; i < W * H; i++) {
		CHECK(idx[i] < colors);
		const uint8_t *p = rgba + i * 4, *q = palette + idx[i] * 4;
		err += abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2]);
		if (q[3] != 255) {
			CHECK(q[3] == 255); // opaque input stays opaque
			break;
		}
	}
	double mean = err / (W * H * 3.0);
	printf("gradient: %d colours, mean error %.2f per channel\n", colors, mean);
	CHECK(mean < 12.0);

	// Few colours, plus transparency: reproduced exactly.
	static const uint8_t swatch[4][4] = {{255, 0, 0, 255}, {0, 128, 255, 255}, {20, 20, 20, 255}, {99, 99, 99, 0}};
	for (int i = 0; i < W * H; i++)
		memcpy(rgba + i * 4, swatch[(i / 37) % 4], 4);
	colors = quant_rgba(rgba, W, H, 0, idx, palette);
	CHECK(colors == 4);
	int exact = 1;
	for (int i = 0; i < W * H; i++) {
		const uint8_t *p = rgba + i * 4, *q = palette + idx[i] * 4;
		if (p[3] == 0 ? q[3] != 0 : memcmp(p, q, 4) != 0)
			exact = 0;
	}
	CHECK(exact);

	// One pixel.
	uint8_t one[4] = {1, 2, 3, 255};
	CHECK(quant_rgba(one, 1, 1, 1, idx, palette) == 1 && memcmp(palette, one, 4) == 0);
	free(rgba);
	free(idx);
}

int main(void) {
	test_livearea();
	test_sfo();
	test_quant();
	if (failures)
		printf("%d failure(s)\n", failures);
	else
		printf("all core tests passed\n");
	return failures ? 1 : 0;
}
