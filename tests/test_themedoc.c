// Host-side tests for the editable theme model, against real theme zips.
// Usage: test_themedoc <scratch dir> <theme.zip>...
#include "../src/at9.h"
#include "../src/fs.h"
#include "../src/imageio.h"
#include "../src/themedoc.h"
#include "../src/unzip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int failures;
#define CHECK(cond)                                                                                \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
			failures++;                                                                            \
		}                                                                                          \
	} while (0)

// Every *FilePath in an output theme must point at a file inside it, unless
// the source theme itself was missing that file.
static int missing_files(const char *dir, const XmlNode *n) {
	int missing = 0;
	for (; n; n = n->next) {
		size_t len = strlen(n->name);
		if (len >= 8 && strcasecmp(n->name + len - 8, "filepath") == 0 && n->text && n->text[0]) {
			char path[640], rel[256];
			xml_get_text(n, rel, sizeof(rel));
			snprintf(path, sizeof(path), "%s/%s", dir, rel);
			if (!fs_exists(path)) {
				printf("    missing %s\n", rel);
				missing++;
			}
		}
		missing += missing_files(dir, n->child);
	}
	return missing;
}

static void check_png(const char *path, int w, int h) {
	uint8_t *px;
	int pw, ph;
	CHECK(image_load_png(path, &px, &pw, &ph) == 0);
	if (pw != w || ph != h)
		printf("    %s is %dx%d, expected %dx%d\n", path, pw, ph, w, h);
	CHECK(pw == w && ph == h);
	free(px);
}

// An 8-bit palette PNG (IHDR colour type 3) of the given size.
static void check_palette_png(const char *path, int w, int h) {
	check_png(path, w, h);
	char *data;
	size_t size;
	CHECK(fs_read_file(path, &data, &size) == 0);
	CHECK(size > 26 && (uint8_t)data[24] == 8 && (uint8_t)data[25] == 3);
	free(data);
}

int main(int argc, char **argv) {
	if (argc < 4) {
		printf("usage: test_themedoc <scratch> <a.zip> <b.zip> [...]\n");
		return 2;
	}
	const char *scratch = argv[1];
	int count = argc - 2;
	char dirs[32][256];
	fs_rm_rf(scratch);
	fs_mkdir_p(scratch);

	for (int i = 0; i < count && i < 32; i++) {
		snprintf(dirs[i], sizeof(dirs[i]), "%s/src%02d", scratch, i);
		CHECK(unzip_extract(argv[i + 2], dirs[i], NULL, NULL, NULL) == UNZIP_OK);
	}

	for (int i = 0; i < count && i < 32; i++) {
		ThemeDoc doc;
		CHECK(themedoc_load(&doc, dirs[i]) == 0);
		if (!doc.root)
			continue;
		char title[128];
		themedoc_get_title(&doc, title, sizeof(title));
		printf("[%02d] %-34.34s pages=%d icons=%2d music=%d lock=%d bar=%d\n", i, title,
		       themedoc_page_count(&doc), themedoc_icon_count(&doc), themedoc_has_part(&doc, PART_MUSIC),
		       themedoc_has_part(&doc, PART_LOCKSCREEN), themedoc_has_part(&doc, PART_STATUSBAR));
		CHECK(title[0]);
		CHECK(themedoc_page_count(&doc) >= 1 && themedoc_page_count(&doc) <= THEMEDOC_MAX_PAGES);

		// Serialising is stable: parse(serialize(x)) serialises identically.
		char *a = xml_serialize(doc.root);
		XmlNode *again = xml_parse(a);
		char *b = xml_serialize(again);
		CHECK(a && b && strcmp(a, b) == 0);
		free(a);
		free(b);
		xml_free(again);

		// An unchanged copy keeps every file the source had.
		char out[300];
		snprintf(out, sizeof(out), "%s/copy%02d", scratch, i);
		char err[160] = "";
		CHECK(themedoc_write(&doc, out, 0, NULL, NULL, err, sizeof(err)) == 0);
		ThemeDoc copy;
		CHECK(themedoc_load(&copy, out) == 0);
		int src_missing = missing_files(dirs[i], doc.root->child);
		int out_missing = copy.root ? missing_files(out, copy.root->child) : 99;
		CHECK(out_missing <= src_missing); // dangling icon references are dropped
		char lock_src[512], lock_out[512];
		themedoc_lockscreen_file(&doc, lock_src, sizeof(lock_src));
		ThemeDoc reread;
		CHECK(themedoc_load(&reread, out) == 0);
		themedoc_lockscreen_file(&reread, lock_out, sizeof(lock_out));
		if (lock_src[0] && fs_exists(lock_src))
			CHECK(lock_out[0] && fs_exists(lock_out) && fs_tree_size(lock_out) == fs_tree_size(lock_src));
		themedoc_free(&reread);
		themedoc_free(&copy);

		// Configurator: music and icons off, second page dropped.
		ThemeDoc edit;
		CHECK(themedoc_copy(&edit, &doc) == 0);
		themedoc_remove_part(&edit, PART_MUSIC);
		themedoc_remove_part(&edit, PART_ICONS);
		CHECK(!themedoc_has_part(&edit, PART_MUSIC));
		CHECK(themedoc_icon_count(&edit) == 0);
		int pages = themedoc_page_count(&edit);
		int keep[THEMEDOC_MAX_PAGES] = {1, 0, 1, 1, 1, 1, 1, 1, 1, 1};
		if (pages > 1) {
			CHECK(themedoc_keep_pages(&edit, keep, pages) == 0);
			CHECK(themedoc_page_count(&edit) == pages - 1);
		}
		int none[THEMEDOC_MAX_PAGES] = {0};
		CHECK(themedoc_keep_pages(&edit, none, THEMEDOC_MAX_PAGES) != 0); // can't remove every page
		themedoc_set_title(&edit, "Custom & \"Quoted\" <title>");
		snprintf(out, sizeof(out), "%s/custom%02d", scratch, i);
		CHECK(themedoc_write(&edit, out, 1, NULL, NULL, err, sizeof(err)) == 0);
		ThemeDoc back;
		CHECK(themedoc_load(&back, out) == 0);
		if (back.root) {
			themedoc_get_title(&back, title, sizeof(title));
			CHECK(strcmp(title, "Custom & \"Quoted\" <title>") == 0);
			CHECK(!themedoc_has_part(&back, PART_MUSIC));
			CHECK(themedoc_icon_count(&back) == 0);
			CHECK(missing_files(out, back.root->child) == 0);
			char p[400];
			snprintf(p, sizeof(p), "%s/preview_page.png", out);
			check_png(p, 480, 272);
			snprintf(p, sizeof(p), "%s/preview_thumbnail.png", out);
			check_png(p, 226, 128);
			snprintf(p, sizeof(p), "%s/preview_lockscreen.png", out);
			check_png(p, 480, 272);
		}
		themedoc_free(&back);
		themedoc_free(&edit);

		// Mixer: every part from the next theme, onto this one.
		ThemeDoc other, mix;
		CHECK(themedoc_load(&other, dirs[(i + 1) % count]) == 0);
		CHECK(themedoc_copy(&mix, &doc) == 0);
		for (int p = 0; p < PART_COUNT; p++)
			if (themedoc_has_part(&other, (ThemePart)p))
				CHECK(themedoc_take_part(&mix, (ThemePart)p, &other) == 0);
		CHECK(themedoc_page_count(&mix) == themedoc_page_count(&other));
		CHECK(themedoc_icon_count(&mix) == themedoc_icon_count(&other));
		CHECK(mix.origin_count == 2);
		char credit[128];
		themedoc_credit(&mix, credit, sizeof(credit));
		snprintf(out, sizeof(out), "%s/mix%02d", scratch, i);
		CHECK(themedoc_write(&mix, out, 1, NULL, NULL, err, sizeof(err)) == 0);
		CHECK(themedoc_load(&back, out) == 0);
		if (back.root) {
			CHECK(missing_files(out, back.root->child) == 0);
			CHECK(themedoc_page_count(&back) == themedoc_page_count(&other));
			if (themedoc_has_part(&other, PART_MUSIC))
				CHECK(themedoc_has_part(&back, PART_MUSIC));
			char lock_other[512], lock_mix[512];
			themedoc_lockscreen_file(&other, lock_other, sizeof(lock_other));
			themedoc_lockscreen_file(&back, lock_mix, sizeof(lock_mix));
			if (lock_other[0] && fs_exists(lock_other))
				CHECK(lock_mix[0] && fs_exists(lock_mix) && fs_tree_size(lock_mix) == fs_tree_size(lock_other));
		}
		printf("     mix with [%02d]: credit '%s'\n", (i + 1) % count, credit);
		themedoc_free(&back);
		themedoc_free(&mix);
		themedoc_free(&other);
		themedoc_free(&doc);
	}

	// Asset library: one page from each of three themes, plus their icon files.
	{
		char out[300], err[160];
		ThemeDoc t[3];
		const ThemeDoc *froms[3];
		int pages[3] = {0, 1, 2}, ok = 1;
		for (int k = 0; k < 3; k++) {
			ok &= themedoc_load(&t[k], dirs[k]) == 0;
			froms[k] = &t[k];
		}
		CHECK(ok);
		ThemeDoc made;
		CHECK(themedoc_copy(&made, &t[0]) == 0);
		CHECK(themedoc_set_pages(&made, froms, pages, 3) == 0);
		CHECK(themedoc_page_count(&made) == 3);
		for (int k = 0; k < 3; k++) {
			char want[512], got[512];
			themedoc_page_file(&t[k], pages[k], 0, want, sizeof(want));
			themedoc_page_file(&made, k, 0, got, sizeof(got));
			CHECK(strcmp(want, got) == 0);
		}
		CHECK(made.origin_count == 3);
		char icons[24][512];
		CHECK(themedoc_icon_files(&t[0], icons, 24) == themedoc_icon_count(&t[0]));
		CHECK(themedoc_set_pages(&made, froms, pages, 0) != 0);
		snprintf(out, sizeof(out), "%s/assets_out", scratch);
		CHECK(themedoc_write(&made, out, 1, NULL, NULL, err, sizeof(err)) == 0);
		ThemeDoc back;
		CHECK(themedoc_load(&back, out) == 0);
		CHECK(back.root && themedoc_page_count(&back) == 3 && missing_files(out, back.root->child) == 0);
		themedoc_free(&back);
		themedoc_free(&made);
		for (int k = 0; k < 3; k++)
			themedoc_free(&t[k]);
	}

	// Pictures from the memory card: a theme made only of pictures, and
	// pictures mixed into a downloaded theme.
	{
		char pics[3][300], out[300], err[200] = "", folder[300];
		static const int sizes[3][2] = {{1280, 720}, {640, 480}, {960, 544}};
		snprintf(folder, sizeof(folder), "%s/picture/SCREENSHOT", scratch);
		fs_mkdir_p(folder);
		snprintf(pics[0], sizeof(pics[0]), "%s/picture/sunset photo.png", scratch);
		snprintf(pics[1], sizeof(pics[1]), "%s/picture/SCREENSHOT/shot&1.png", scratch);
		snprintf(pics[2], sizeof(pics[2]), "%s/picture/SCREENSHOT/sunset photo.png", scratch); // same name
		for (int k = 0; k < 3; k++) {
			int w = sizes[k][0], h = sizes[k][1];
			uint8_t *px = malloc((size_t)w * h * 4);
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++) {
					uint8_t *p = px + ((size_t)y * w + x) * 4;
					p[0] = (uint8_t)(x * 255 / w), p[1] = (uint8_t)(y * 255 / h), p[2] = (uint8_t)(k * 100), p[3] = 255;
				}
			CHECK(image_save_png(pics[k], px, w, h) == 0);
			free(px);
		}
		const char *list[3] = {pics[0], pics[1], pics[2]};
		ThemeDoc pic;
		CHECK(themedoc_from_pictures(&pic, list, 3, pics[1]) == 0);
		CHECK(themedoc_page_count(&pic) == 3 && themedoc_has_part(&pic, PART_LOCKSCREEN));
		CHECK(themedoc_has_part(&pic, PART_STATUSBAR));
		CHECK(themedoc_uses_pictures(&pic, PART_WALLPAPERS) && themedoc_uses_pictures(&pic, PART_LOCKSCREEN));
		CHECK(pic.origin_count == 2 && pic.origin_pictures[0] && pic.origin_pictures[1]);
		char page[512];
		themedoc_page_file(&pic, 1, 0, page, sizeof(page));
		CHECK(strcmp(page, pics[1]) == 0);
		char credit[128];
		themedoc_credit(&pic, credit, sizeof(credit));
		CHECK(credit[0] == '\0'); // your own pictures aren't credited to anyone
		CHECK(themedoc_from_pictures(&(ThemeDoc){0}, list, 0, NULL) != 0);

		snprintf(out, sizeof(out), "%s/pictures_out", scratch);
		CHECK(themedoc_write(&pic, out, 1, NULL, NULL, err, sizeof(err)) == 0);
		ThemeDoc back;
		CHECK(themedoc_load(&back, out) == 0);
		CHECK(back.root && themedoc_page_count(&back) == 3 && missing_files(out, back.root->child) == 0);
		for (int k = 0; back.root && k < 3; k++) {
			char wall[512], thumb[512];
			themedoc_page_file(&back, k, 0, wall, sizeof(wall));
			themedoc_page_file(&back, k, 1, thumb, sizeof(thumb));
			check_palette_png(wall, THEME_WALLPAPER_W, THEME_WALLPAPER_H);
			check_palette_png(thumb, THEME_PAGE_THUMB_W, THEME_PAGE_THUMB_H);
			CHECK(strstr(wall, ".png") && !strchr(strrchr(wall, '/') + 1, ' ') && !strchr(wall, '&'));
		}
		char lock[512];
		themedoc_lockscreen_file(&back, lock, sizeof(lock));
		check_palette_png(lock, THEME_WALLPAPER_W, THEME_WALLPAPER_H);
		char p[400];
		snprintf(p, sizeof(p), "%s/preview_thumbnail.png", out);
		check_png(p, 226, 128);
		themedoc_free(&back);

		// Mixed: the first sample theme keeps its first page, gains two
		// pictures, and takes a picture as its lock screen (keeping its colours).
		ThemeDoc base, mixed;
		CHECK(themedoc_load(&base, dirs[0]) == 0);
		CHECK(themedoc_copy(&mixed, &base) == 0);
		CHECK(!themedoc_uses_pictures(&mixed, PART_WALLPAPERS) && !themedoc_uses_pictures(&mixed, PART_LOCKSCREEN));
		const ThemeDoc *froms[3] = {&base, &pic, &pic};
		int pages[3] = {0, 0, 2};
		CHECK(themedoc_set_pages(&mixed, froms, pages, 3) == 0);
		ThemeDoc lockpic;
		const char *one[1] = {pics[1]};
		CHECK(themedoc_from_pictures(&lockpic, one, 0, one[0]) == 0);
		char color_before[32], color_after[32];
		xml_get_text(xml_find(mixed.root, "StartScreenProperty/m_dateColor"), color_before, sizeof(color_before));
		CHECK(themedoc_take_lockscreen_image(&mixed, &lockpic) == 0);
		CHECK(themedoc_uses_pictures(&mixed, PART_WALLPAPERS) && themedoc_uses_pictures(&mixed, PART_LOCKSCREEN));
		xml_get_text(xml_find(mixed.root, "StartScreenProperty/m_dateColor"), color_after, sizeof(color_after));
		CHECK(strcmp(color_before, color_after) == 0);
		themedoc_lockscreen_file(&mixed, lock, sizeof(lock));
		CHECK(strcmp(lock, pics[1]) == 0);
		snprintf(out, sizeof(out), "%s/pictures_mixed", scratch);
		CHECK(themedoc_write(&mixed, out, 1, NULL, NULL, err, sizeof(err)) == 0);
		CHECK(themedoc_load(&back, out) == 0);
		CHECK(back.root && themedoc_page_count(&back) == 3 && missing_files(out, back.root->child) == 0);
		if (back.root) {
			char want[512], got[512];
			themedoc_page_file(&base, 0, 0, want, sizeof(want));
			themedoc_page_file(&back, 0, 0, got, sizeof(got));
			CHECK(fs_tree_size(want) == fs_tree_size(got)); // the theme's own page is copied as-is
			themedoc_page_file(&back, 2, 0, got, sizeof(got));
			check_palette_png(got, THEME_WALLPAPER_W, THEME_WALLPAPER_H);
			themedoc_lockscreen_file(&back, got, sizeof(got));
			check_palette_png(got, THEME_WALLPAPER_W, THEME_WALLPAPER_H);
		}
		themedoc_free(&back);

		// A picture with very few colours still comes out as an 8-bit PNG.
		{
			uint8_t *solid = malloc(64 * 32 * 4);
			for (int k = 0; k < 64 * 32; k++)
				solid[k * 4] = 200, solid[k * 4 + 1] = 30, solid[k * 4 + 2] = 40, solid[k * 4 + 3] = 255;
			snprintf(p, sizeof(p), "%s/solid.png", scratch);
			CHECK(image_save_png_indexed(p, solid, 64, 32) == 0);
			check_palette_png(p, 64, 32);
			free(solid);
		}

		// A file that isn't a picture fails the write cleanly.
		snprintf(p, sizeof(p), "%s/picture/notes.png", scratch);
		fs_write_file(p, "not a picture", 13);
		const char *bad[1] = {p};
		ThemeDoc broken;
		CHECK(themedoc_from_pictures(&broken, bad, 1, NULL) == 0);
		snprintf(out, sizeof(out), "%s/pictures_broken", scratch);
		CHECK(themedoc_write(&broken, out, 1, NULL, NULL, err, sizeof(err)) != 0);
		CHECK(strstr(err, "notes.png") != NULL && !fs_exists(out));
		themedoc_free(&broken);
		themedoc_free(&lockpic);
		themedoc_free(&mixed);
		themedoc_free(&base);
		themedoc_free(&pic);
	}

	// Every sample theme's music parses as ATRAC9.
	for (int i = 0; i < count && i < 32; i++) {
		ThemeDoc t;
		if (themedoc_load(&t, dirs[i]) != 0)
			continue;
		char music[512];
		themedoc_music_file(&t, music, sizeof(music));
		if (music[0] && fs_exists(music)) {
			char *data;
			size_t size;
			At9Info info;
			CHECK(fs_read_file(music, &data, &size) == 0);
			CHECK(at9_parse((const uint8_t *)data, size, &info) == 0);
			if (i == 0)
				printf("at9: %d ch, %d Hz, block %d, data %u bytes\n", info.channels, info.sample_rate,
				       info.block_align, (unsigned)info.data_size);
			CHECK(info.data_offset + info.data_size <= size);
			free(data);
		}
		themedoc_free(&t);
	}
	At9Info junk;
	CHECK(at9_parse((const uint8_t *)"RIFF\x10\0\0\0WAVEdata", 16, &junk) != 0);

	// A crafted theme can't make Imago copy files from outside its folder.
	char evil[300], out[300], err[160];
	snprintf(evil, sizeof(evil), "%s/evil", scratch);
	fs_mkdir_p(evil);
	char path[400];
	snprintf(path, sizeof(path), "%s/theme.xml", evil);
	const char *xml = "<theme format-ver=\"01.00\" package=\"0\"><InfomationProperty><m_title><m_default>Evil"
	                  "</m_default></m_title></InfomationProperty><HomeProperty><m_bgParam><BackgroundParam>"
	                  "<m_imageFilePath>../../secret.db</m_imageFilePath></BackgroundParam></m_bgParam>"
	                  "<m_bgmFilePath>ur0:shell/db/app.db</m_bgmFilePath></HomeProperty></theme>";
	fs_write_file(path, xml, strlen(xml));
	snprintf(path, sizeof(path), "%s/secret.db", scratch);
	fs_write_file(path, "secret", 6);
	ThemeDoc doc;
	CHECK(themedoc_load(&doc, evil) == 0);
	char file[512];
	CHECK(themedoc_page_file(&doc, 0, 0, file, sizeof(file)) != 0);
	snprintf(out, sizeof(out), "%s/evil_out", scratch);
	CHECK(themedoc_write(&doc, out, 0, NULL, NULL, err, sizeof(err)) == 0);
	snprintf(path, sizeof(path), "%s/secret.db", out);
	CHECK(!fs_exists(path));
	snprintf(path, sizeof(path), "%s/app.db", out);
	CHECK(!fs_exists(path));
	themedoc_free(&doc);

	printf(failures ? "%d FAILURES\n" : "all tests passed\n", failures);
	return failures != 0;
}
