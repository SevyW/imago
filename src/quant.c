#include "quant.h"

#include <stdlib.h>
#include <string.h>

// Colours are bucketed to 5 bits of red, green and blue and 3 bits of alpha
// (an 18-bit key). Each bucket keeps the exact sum of its pixels, so palette
// entries are true averages rather than bucket centres.
#define KEY_BITS 18
#define KEYS (1 << KEY_BITS)
#define MAX_COLORS 256

typedef struct {
	uint32_t key;
	uint32_t count;
} Entry;

typedef struct {
	int start, end; // range in the entry array
	uint32_t pixels;
	int channel;  // the widest channel
	int range;    // its spread, in weighted units
} Box;

static uint32_t key_of(int r, int g, int b, int a) {
	return (uint32_t)(r >> 3) << 13 | (uint32_t)(g >> 3) << 8 | (uint32_t)(b >> 3) << 3 | (uint32_t)(a >> 5);
}

// Channel value of a key, scaled so one step weighs about the same in each
// channel (alpha has fewer bits but matters as much).
static int channel_of(uint32_t key, int channel) {
	switch (channel) {
	case 0: return (int)((key >> 13) & 31) * 8;
	case 1: return (int)((key >> 8) & 31) * 8;
	case 2: return (int)((key >> 3) & 31) * 8;
	default: return (int)(key & 7) * 36;
	}
}

static void measure(Box *box, const Entry *entries) {
	int lo[4] = {255 * 2, 255 * 2, 255 * 2, 255 * 2}, hi[4] = {0, 0, 0, 0};
	box->pixels = 0;
	for (int i = box->start; i < box->end; i++) {
		for (int c = 0; c < 4; c++) {
			int v = channel_of(entries[i].key, c);
			if (v < lo[c])
				lo[c] = v;
			if (v > hi[c])
				hi[c] = v;
		}
		box->pixels += entries[i].count;
	}
	box->range = -1;
	for (int c = 0; c < 4; c++) {
		if (hi[c] - lo[c] > box->range) {
			box->range = hi[c] - lo[c];
			box->channel = c;
		}
	}
}

// Stable counting sort of a box's entries by one channel (at most 32 values).
static void sort_box(const Box *box, Entry *entries, Entry *scratch) {
	int counts[33] = {0};
	int shift = box->channel == 0 ? 13 : box->channel == 1 ? 8 : box->channel == 2 ? 3 : 0;
	uint32_t mask = box->channel == 3 ? 7 : 31;
	for (int i = box->start; i < box->end; i++)
		counts[((entries[i].key >> shift) & mask) + 1]++;
	for (int v = 1; v < 33; v++)
		counts[v] += counts[v - 1];
	for (int i = box->start; i < box->end; i++)
		scratch[counts[(entries[i].key >> shift) & mask]++] = entries[i];
	memcpy(entries + box->start, scratch, sizeof(Entry) * (size_t)(box->end - box->start));
}

static int split(Box *boxes, int count, Entry *entries, Entry *scratch) {
	// Split the box whose widest channel spread, weighted by its pixels, is largest.
	int best = -1;
	double best_score = 0.0;
	for (int i = 0; i < count; i++) {
		if (boxes[i].end - boxes[i].start < 2 || boxes[i].range <= 0)
			continue;
		double score = (double)boxes[i].range * (double)boxes[i].pixels;
		if (score > best_score) {
			best_score = score;
			best = i;
		}
	}
	if (best < 0)
		return 0;
	Box *box = &boxes[best];
	sort_box(box, entries, scratch);
	uint32_t half = box->pixels / 2, seen = 0;
	int cut = box->start;
	while (cut < box->end - 1 && seen + entries[cut].count <= half)
		seen += entries[cut++].count;
	if (cut == box->start)
		cut++;
	Box *other = &boxes[count];
	other->start = cut;
	other->end = box->end;
	box->end = cut;
	measure(box, entries);
	measure(other, entries);
	return 1;
}

static int nearest(const uint8_t *palette, int colors, int r, int g, int b, int a) {
	int best = 0;
	long best_d = -1;
	for (int i = 0; i < colors; i++) {
		const uint8_t *p = palette + i * 4;
		long dr = r - p[0], dg = g - p[1], db = b - p[2], da = a - p[3];
		long d = dr * dr + dg * dg + db * db + da * da * 2;
		if (best_d < 0 || d < best_d) {
			best_d = d;
			best = i;
		}
	}
	return best;
}

static int clamp(int v) {
	return v < 0 ? 0 : v > 255 ? 255 : v;
}

int quant_rgba(const uint8_t *rgba, int w, int h, int dither, uint8_t *indices, uint8_t *palette) {
	size_t pixels = (size_t)w * (size_t)h;
	uint32_t *sums = calloc((size_t)KEYS * 5, sizeof(uint32_t)); // count, r, g, b, a per key
	uint16_t *cache = calloc(KEYS, sizeof(uint16_t));                // palette index + 1
	int *err = dither ? calloc((size_t)(w + 2) * 2 * 3, sizeof(int)) : NULL;
	Entry *entries = NULL, *scratch = NULL;
	int colors = -1;
	if (!sums || !cache || (dither && !err))
		goto done;

	// Fully transparent pixels all become one colour.
	for (size_t i = 0; i < pixels; i++) {
		const uint8_t *p = rgba + i * 4;
		int r = p[0], g = p[1], b = p[2], a = p[3];
		if (a == 0)
			r = g = b = 0;
		uint32_t *s = sums + (size_t)key_of(r, g, b, a) * 5;
		s[0]++, s[1] += (uint32_t)r, s[2] += (uint32_t)g, s[3] += (uint32_t)b, s[4] += (uint32_t)a;
	}
	int unique = 0;
	for (uint32_t k = 0; k < KEYS; k++)
		unique += sums[(size_t)k * 5] != 0;
	entries = malloc(sizeof(Entry) * (size_t)(unique ? unique : 1));
	scratch = malloc(sizeof(Entry) * (size_t)(unique ? unique : 1));
	if (!entries || !scratch)
		goto done;
	int n = 0;
	for (uint32_t k = 0; k < KEYS; k++)
		if (sums[(size_t)k * 5])
			entries[n++] = (Entry){k, sums[(size_t)k * 5]};

	Box boxes[MAX_COLORS];
	int count = 0;
	if (n > 0) {
		boxes[0] = (Box){0, n, 0, 0, 0};
		measure(&boxes[0], entries);
		count = 1;
		while (count < MAX_COLORS && split(boxes, count, entries, scratch))
			count++;
	}
	for (int i = 0; i < count; i++) {
		uint64_t t[5] = {0, 0, 0, 0, 0};
		for (int e = boxes[i].start; e < boxes[i].end; e++)
			for (int c = 0; c < 5; c++)
				t[c] += sums[(size_t)entries[e].key * 5 + c];
		for (int c = 0; c < 4; c++)
			palette[i * 4 + c] = (uint8_t)((t[c + 1] + t[0] / 2) / t[0]);
	}
	if (count == 0) { // empty image
		memset(palette, 0, 4);
		count = 1;
	}

	for (int y = 0; y < h; y++) {
		int *cur = err ? err + (y & 1) * (w + 2) * 3 : NULL;
		int *next = err ? err + ((y + 1) & 1) * (w + 2) * 3 : NULL;
		if (next)
			memset(next, 0, sizeof(int) * (size_t)(w + 2) * 3);
		for (int x = 0; x < w; x++) {
			const uint8_t *p = rgba + ((size_t)y * w + x) * 4;
			int r = p[0], g = p[1], b = p[2], a = p[3];
			if (a == 0) {
				r = g = b = 0;
			} else if (cur) {
				// Errors are stored in sixteenths.
				r = clamp(r + cur[(x + 1) * 3] / 16);
				g = clamp(g + cur[(x + 1) * 3 + 1] / 16);
				b = clamp(b + cur[(x + 1) * 3 + 2] / 16);
			}
			uint32_t key = key_of(r, g, b, a);
			int index = cache[key] ? cache[key] - 1 : -1;
			if (index < 0) {
				index = nearest(palette, count, r, g, b, a);
				cache[key] = (uint16_t)(index + 1);
			}
			indices[(size_t)y * w + x] = (uint8_t)index;
			if (cur && a != 0) {
				const uint8_t *q = palette + index * 4;
				int e[3] = {r - q[0], g - q[1], b - q[2]};
				for (int c = 0; c < 3; c++) {
					cur[(x + 2) * 3 + c] += e[c] * 7;
					next[x * 3 + c] += e[c] * 3;
					next[(x + 1) * 3 + c] += e[c] * 5;
					next[(x + 2) * 3 + c] += e[c];
				}
			}
		}
	}
	colors = count;
done:
	free(sums);
	free(cache);
	free(err);
	free(entries);
	free(scratch);
	return colors;
}
