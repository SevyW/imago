// Host-side tests for the psvt.ovh scraper.
// Usage: test_psvt <list.html> <detail.html>
#include "../src/psvt.h"

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

static char *read_file(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)n + 1);
	fread(buf, 1, (size_t)n, f);
	buf[n] = '\0';
	fclose(f);
	return buf;
}

static void test_urls(void) {
	char url[512];
	PsvtQuery q = {.sort = PSVT_SORT_DOWNLOADS};
	strcpy(q.search, "zelda & link");
	strcpy(q.tag, "hack and slash");
	psvt_list_url(url, sizeof(url), &q, 3);
	CHECK(strcmp(url, "http://psvt.ovh/index.php?m=list&p=3&o=downloads&s=zelda+%26+link&t=hack+and+slash") == 0);
	psvt_preview_url(url, sizeof(url), "abc", 216);
	CHECK(strstr(url, "wsrv.nl/?url=psvt.ovh/themes/abc.png&w=216") != NULL);
}

static void test_text(void) {
	char out[64];
	const char *s = "  Marvel&#039;s  Spider-Man&amp;Co &eacute;x &#x263A; ";
	psvt_html_text(out, sizeof(out), s, strlen(s));
	CHECK(strcmp(out, "Marvel's Spider-Man&Co &eacute;x \xE2\x98\xBA") == 0);
}

static void test_list(const char *path) {
	char *html = read_file(path);
	CHECK(html != NULL);
	if (!html)
		return;
	static PsvtPage page;
	int n = psvt_parse_list(html, &page);
	printf("list: %d themes, %d pages\n", n, page.total_pages);
	CHECK(n >= 20);
	CHECK(page.total_pages > 100);
	for (int i = 0; i < page.count; i++) {
		PsvtTheme *t = &page.items[i];
		CHECK(strlen(t->hash) == 40);
		CHECK(t->id > 1400000000u);
		CHECK(t->downloads >= 0);
		if (i < 3)
			printf("  #%u %s [%s] %d dl valid=%d\n    %s\n", t->id, t->title, t->hash, t->downloads,
			       t->valid, t->subtitle);
	}
	free(html);
}

static void test_detail(const char *path) {
	char *html = read_file(path);
	CHECK(html != NULL);
	if (!html)
		return;
	// The app stops downloading at the marker, so test on a truncated page.
	char *marker = strstr(html, PSVT_DETAIL_END_MARKER);
	CHECK(marker != NULL);
	if (marker)
		marker[strlen(PSVT_DETAIL_END_MARKER)] = '\0';
	PsvtDetail d;
	CHECK(psvt_parse_detail(html, &d) == 0);
	printf("detail: '%s' v%s by '%s' score=%s (%d/%d/%d) dl=%d added='%s' updated='%s' hash=%s\n",
	       d.title, d.version, d.author, d.score, d.valids, d.warnings, d.errors, d.downloads, d.added,
	       d.updated, d.hash);
	for (int i = 0; i < d.tag_count; i++)
		printf("  tag: %s\n", d.tags[i]);
	CHECK(strcmp(d.version, "01.00") == 0);
	CHECK(d.author[0] != '\0');
	CHECK(d.valids > 0);
	CHECK(d.tag_count > 0);
	CHECK(strlen(d.hash) == 40);
	free(html);
}

int main(int argc, char **argv) {
	test_urls();
	test_text();
	if (argc > 2) {
		test_list(argv[1]);
		test_detail(argv[2]);
	}
	printf(failures ? "%d FAILURES\n" : "all tests passed\n", failures);
	return failures != 0;
}
