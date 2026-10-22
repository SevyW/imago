#include "edit.h"

#include "appdb.h"
#include "fs.h"
#include "imageio.h"
#include "livearea.h"
#include "quant.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_message(char *err, const char *message) {
	if (err)
		snprintf(err, EDIT_ERROR_SIZE, "%s", message);
}

// The first file a part currently uses in the app's appmeta folder.
static int current_file(const char *id, ArtPart part, char *out, size_t size) {
	if (part == ART_ICON) {
		store_appmeta_path(id, "icon0.png", out, size);
		return 0;
	}
	LiveArea la;
	if (appdb_livearea(id, &la) != 0)
		return -1;
	const char *name = part == ART_BG ? (la.bg_count ? la.bg[0] : NULL) : (la.gate_count ? la.gate[0] : NULL);
	if (!name)
		return -1;
	char rel[LIVEAREA_NAME_SIZE + 32];
	snprintf(rel, sizeof(rel), "livearea/contents/%s", name);
	store_appmeta_path(id, rel, out, size);
	return 0;
}

void edit_target_size(const char *id, ArtPart part, int *w, int *h) {
	static const int defaults[ART_COUNT][2] = {{ICON_W, ICON_H}, {BG_W, BG_H}, {GATE_W, GATE_H}};
	*w = defaults[part][0];
	*h = defaults[part][1];
	char path[512];
	int cw, ch;
	if (current_file(id, part, path, sizeof(path)) != 0 || image_png_size(path, &cw, &ch) != 0)
		return;
	// Keep the app's own size (retail games use e.g. 840x512 or 960x572) unless it is implausible.
	if (cw >= 32 && ch >= 32 && cw <= 1024 && ch <= 1024) {
		*w = cw;
		*h = ch;
	}
}

// Saves to a temporary name first so a failed write never leaves half a file.
static int save_indexed(const char *path, const uint8_t *indices, int w, int h, const uint8_t *palette, int colors) {
	char temp[600];
	snprintf(temp, sizeof(temp), "%s.tmp", path);
	if (image_save_png_palette(temp, indices, w, h, palette, colors) != 0) {
		remove(temp);
		return -1;
	}
	remove(path);
	return fs_rename(temp, path);
}

int edit_make(const char *id, ArtPart part, const char *source, Crop crop, int seamless, int gate_x, int gate_y,
              char *err) {
	if (!store_valid_id(id)) {
		set_message(err, "Nothing to make.");
		return -1;
	}
	uint8_t *src = NULL, *pixels = NULL, *indices = NULL, *gate = NULL;
	uint8_t palette[256 * 4];
	int sw, sh, rc = -1;
	if (image_load_limited(source, &src, &sw, &sh, EDIT_MAX_SOURCE_PIXELS) != 0) {
		set_message(err, "Could not open that picture. Imago reads PNG and JPEG files up to 2048x2048.");
		return -1;
	}
	int w, h;
	edit_target_size(id, part, &w, &h);
	pixels = image_resize_region(src, sw, sh, crop.x * sw, crop.y * sh, crop.w * sw, crop.h * sh, w, h);
	free(src);
	indices = malloc((size_t)w * h);
	int colors = pixels && indices ? quant_rgba(pixels, w, h, 1, indices, palette) : -1;
	if (colors < 0) {
		set_message(err, "Not enough memory to prepare the picture.");
		goto done;
	}

	char dir[512], path[512];
	snprintf(dir, sizeof(dir), STORE_DIR "/%s/custom", id);
	store_custom_path(id, part, path, sizeof(path));
	if (fs_mkdir_p(dir) != 0 || save_indexed(path, indices, w, h, palette, colors) != 0) {
		set_message(err, "Could not save the picture (is the memory card full?).");
		goto done;
	}

	if (part == ART_BG && seamless) {
		// The gate's own size, at the background's scale.
		int gw, gh;
		edit_target_size(id, ART_GATE, &gw, &gh);
		int gx = gate_x * w / BG_W, gy = gate_y * h / BG_H;
		if (gx + gw > w)
			gx = w - gw;
		if (gy + gh > h)
			gy = h - gh;
		if (gx < 0 || gy < 0) {
			set_message(err, "The start gate doesn't fit inside this background.");
			goto done;
		}
		// Cutting the palettised background keeps the gate pixel-identical to it.
		gate = malloc((size_t)gw * gh);
		if (!gate) {
			set_message(err, "Not enough memory to prepare the start gate.");
			goto done;
		}
		for (int y = 0; y < gh; y++)
			memcpy(gate + (size_t)y * gw, indices + (size_t)(gy + y) * w + gx, (size_t)gw);
		store_custom_path(id, ART_GATE, path, sizeof(path));
		if (save_indexed(path, gate, gw, gh, palette, colors) != 0) {
			set_message(err, "Could not save the start gate (is the memory card full?).");
			goto done;
		}
	}
	rc = 0;
done:
	free(pixels);
	free(indices);
	free(gate);
	return rc;
}
