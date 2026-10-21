// psvt.ovh repository client: URL building and HTML scraping.
// Pure C, no Vita dependencies, so it can be unit-tested on a PC.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define PSVT_HOST "http://psvt.ovh"
#define PSVT_MAX_PAGE_ITEMS 32
#define PSVT_MAX_TAGS 16

typedef enum {
	PSVT_SORT_NEWEST,
	PSVT_SORT_TRENDING,
	PSVT_SORT_POPULAR,
	PSVT_SORT_DOWNLOADS,
	PSVT_SORT_TITLE,
	PSVT_SORT_AUTHOR,
	PSVT_SORT_COUNT
} PsvtSort;

typedef struct {
	uint32_t id;      // theme.php?id=... (also the unix time it was added)
	char hash[41];    // sha1 used for both the preview image and the download
	char title[128];
	char subtitle[192]; // tags or a short description
	int downloads;      // -1 when unknown
	int valid;          // 1 when the site shows the "100% Valid" badge
} PsvtTheme;

typedef struct {
	PsvtTheme items[PSVT_MAX_PAGE_ITEMS];
	int count;
	int total_pages;
} PsvtPage;

typedef struct {
	char title[128];
	char version[16];
	char author[64];
	char score[16];   // e.g. "100,00%"
	int valids, warnings, errors;
	int downloads;
	char added[32];
	char updated[32];
	char tags[PSVT_MAX_TAGS][48];
	int tag_count;
	char hash[41];
} PsvtDetail;

// Filter is optional: pass NULL or "" for search/tag/author to ignore them.
typedef struct {
	PsvtSort sort;
	char search[64];
	char tag[48];
	char author[64];
} PsvtQuery;

const char *psvt_sort_name(PsvtSort sort);

void psvt_list_url(char *out, size_t size, const PsvtQuery *query, int page);
void psvt_detail_url(char *out, size_t size, uint32_t id);
void psvt_download_url(char *out, size_t size, const char *hash);
// Resized JPEG preview through the wsrv.nl proxy (~8 KB instead of ~200 KB).
void psvt_preview_url(char *out, size_t size, const char *hash, int width);

// Returns the number of themes parsed, or -1 if the page is not recognised.
int psvt_parse_list(const char *html, PsvtPage *page);
// Returns 0 on success. Works on a truncated page (only the header is needed).
int psvt_parse_detail(const char *html, PsvtDetail *detail);
// Marker that ends the useful part of a detail page; everything after it is
// megabytes of inline base64 images, so downloads can stop there.
#define PSVT_DETAIL_END_MARKER "themeAssetsAccordion"

// Decodes HTML entities and trims/collapses whitespace into out (UTF-8).
void psvt_html_text(char *out, size_t size, const char *src, size_t len);
