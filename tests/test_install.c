// Host-side tests for zip extraction and theme metadata.
// Usage: test_install <theme.zip> <nested.zip> <scratch dir>
#include "../src/fs.h"
#include "../src/theme.h"
#include "../src/unzip.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
			failures++;                                                                            \
		}                                                                                          \
	} while (0)

static int last_percent = -1;
static int on_progress(void *user, uint64_t done, uint64_t total) {
	(void)user;
	int pct = total ? (int)(done * 100 / total) : 0;
	CHECK(pct >= last_percent);
	last_percent = pct;
	return 0;
}

static int cancel_now(void *user, uint64_t done, uint64_t total) {
	(void)user;
	(void)done;
	(void)total;
	return 1;
}

static void test_extract(const char *zip, const char *dir) {
	fs_rm_rf(dir);
	uint64_t bytes = 0;
	int rc = unzip_extract(zip, dir, on_progress, NULL, &bytes);
	printf("extract %s -> %d (%s), %llu bytes\n", zip, rc, unzip_strerror(rc), (unsigned long long)bytes);
	CHECK(rc == UNZIP_OK);
	CHECK(last_percent == 100);
	CHECK((int64_t)bytes == fs_tree_size(dir));

	ThemeXml xml;
	CHECK(theme_xml_load(dir, &xml) == 0);
	printf("  title='%s' provider='%s' ver=%s pkg=%s home=%s start=%s\n", xml.title, xml.provider,
	       xml.content_ver, xml.package_image, xml.home_preview, xml.start_preview);
	CHECK(theme_content_version(xml.content_ver) == 100);
	CHECK(strcmp(xml.package_image, "preview_thumbnail.png") == 0);
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, xml.home_preview);
	CHECK(fs_exists(path));

	ThemeMarker m = {1792556293u, "374f14d8d0155839ae198089ad7c32f4ec2eed8b"}, r;
	CHECK(theme_marker_write(dir, &m) == 0);
	CHECK(theme_marker_read(dir, &r) == 0);
	CHECK(r.id == m.id && strcmp(r.hash, m.hash) == 0);

	CHECK(fs_rm_rf(dir) == 0);
	CHECK(!fs_exists(dir));
}

int main(int argc, char **argv) {
	char name[64];
	theme_folder_name(name, sizeof(name), "  Zelda: Breath of the Wild!! ");
	CHECK(strcmp(name, "Zelda_Breath_of_the_Wild") == 0);
	theme_folder_name(name, sizeof(name), "\xE3\x81\x82\xE3\x81\x84");
	CHECK(strcmp(name, "theme") == 0);

	CHECK(theme_dir_is_safe("ux0:customtheme/Foo"));
	CHECK(theme_dir_is_safe("uma0:customtheme/Foo_123"));
	CHECK(!theme_dir_is_safe("ux0:customtheme/"));
	CHECK(!theme_dir_is_safe("ux0:customtheme/.."));
	CHECK(!theme_dir_is_safe("ux0:customtheme/Foo/bar"));
	CHECK(!theme_dir_is_safe("ux0:customtheme"));
	CHECK(!theme_dir_is_safe("ux0:theme/PCSB00000"));
	CHECK(!theme_dir_is_safe("ux0:data/customtheme/Foo"));
	CHECK(!theme_dir_is_safe("ur0:customtheme/a:b"));

	if (argc > 3) {
		char dir[512];
		snprintf(dir, sizeof(dir), "%s/out_flat", argv[3]);
		last_percent = -1;
		test_extract(argv[1], dir);
		snprintf(dir, sizeof(dir), "%s/out_nested", argv[3]);
		last_percent = -1;
		test_extract(argv[2], dir);
		CHECK(unzip_extract(argv[1], dir, cancel_now, NULL, NULL) == UNZIP_ERR_CANCELED);
		fs_rm_rf(dir);
	}
	printf(failures ? "%d FAILURES\n" : "all tests passed\n", failures);
	return failures != 0;
}
