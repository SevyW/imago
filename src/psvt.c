#include "psvt.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const sort_params[PSVT_SORT_COUNT] = {
	"id", "trending", "popular", "downloads", "title", "author",
};

static const char *const sort_names[PSVT_SORT_COUNT] = {
	"Newest", "Trending", "Popular", "Most downloaded", "Title", "Author",
};

const char *psvt_sort_name(PsvtSort sort) {
	return sort < PSVT_SORT_COUNT ? sort_names[sort] : "";
}

// application/x-www-form-urlencoded, as the site's own search form sends it
static size_t url_encode(char *out, size_t size, const char *src) {
	static const char hex[] = "0123456789ABCDEF";
	size_t n = 0;
	for (; *src && n + 4 < size; src++) {
		unsigned char c = (unsigned char)*src;
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out[n++] = c;
		} else if (c == ' ') {
			out[n++] = '+';
		} else {
			out[n++] = '%';
			out[n++] = hex[c >> 4];
			out[n++] = hex[c & 15];
		}
	}
	out[n] = '\0';
	return n;
}

static void append_param(char *out, size_t size, const char *key, const char *value) {
	if (!value || !value[0])
		return;
	char encoded[256];
	url_encode(encoded, sizeof(encoded), value);
	size_t len = strlen(out);
	snprintf(out + len, size - len, "&%s=%s", key, encoded);
}

void psvt_list_url(char *out, size_t size, const PsvtQuery *query, int page) {
	PsvtSort sort = query->sort < PSVT_SORT_COUNT ? query->sort : PSVT_SORT_NEWEST;
	snprintf(out, size, PSVT_HOST "/index.php?m=list&p=%d&o=%s", page, sort_params[sort]);
	append_param(out, size, "s", query->search);
	append_param(out, size, "t", query->tag);
	append_param(out, size, "a", query->author);
}

void psvt_detail_url(char *out, size_t size, uint32_t id) {
	snprintf(out, size, PSVT_HOST "/theme.php?id=%u", (unsigned)id);
}

void psvt_download_url(char *out, size_t size, const char *hash) {
	snprintf(out, size, PSVT_HOST "/download.php?id=%s", hash);
}

void psvt_preview_url(char *out, size_t size, const char *hash, int width) {
	snprintf(out, size, "http://wsrv.nl/?url=psvt.ovh/themes/%s.png&w=%d&output=jpg&q=82&maxage=1y",
	         hash, width);
}

// ---------------------------------------------------------------------------
// HTML helpers

static size_t put_utf8(char *out, uint32_t cp) {
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

static const struct {
	const char *name;
	uint32_t cp;
} named_entities[] = {
	{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
	{"nbsp", ' '}, {"hellip", 0x2026}, {"ndash", 0x2013}, {"mdash", 0x2014},
	{"rsquo", 0x2019}, {"lsquo", 0x2018}, {"rdquo", 0x201D}, {"ldquo", 0x201C},
};

// Parses an entity at src (which points at '&'); returns its length or 0.
static size_t decode_entity(const char *src, size_t len, uint32_t *cp) {
	size_t end = 1;
	while (end < len && end < 12 && src[end] != ';')
		end++;
	if (end >= len || src[end] != ';')
		return 0;
	if (src[1] == '#') {
		char *stop;
		unsigned long v = (src[2] == 'x' || src[2] == 'X') ? strtoul(src + 3, &stop, 16)
		                                                   : strtoul(src + 2, &stop, 10);
		if (stop != src + end || v == 0 || v > 0x10FFFF)
			return 0;
		*cp = (uint32_t)v;
		return end + 1;
	}
	for (size_t i = 0; i < sizeof(named_entities) / sizeof(named_entities[0]); i++) {
		size_t n = strlen(named_entities[i].name);
		if (n == end - 1 && memcmp(src + 1, named_entities[i].name, n) == 0) {
			*cp = named_entities[i].cp;
			return end + 1;
		}
	}
	return 0;
}

void psvt_html_text(char *out, size_t size, const char *src, size_t len) {
	size_t n = 0;
	int pending_space = 0;
	for (size_t i = 0; i < len && n + 5 < size;) {
		char c = src[i];
		if (c == '<') { // drop inline tags such as <i class="bi ...">
			while (i < len && src[i] != '>')
				i++;
			i++;
			pending_space = 1;
			continue;
		}
		if (isspace((unsigned char)c)) {
			pending_space = 1;
			i++;
			continue;
		}
		if (pending_space && n > 0)
			out[n++] = ' ';
		pending_space = 0;
		uint32_t cp;
		size_t used = c == '&' ? decode_entity(src + i, len - i, &cp) : 0;
		if (used) {
			n += put_utf8(out + n, cp);
			i += used;
		} else {
			out[n++] = c;
			i++;
		}
	}
	out[n] = '\0';
}

// Finds `open` at or after p (before limit), then copies the text up to `close`.
// Returns a pointer just past the closing marker, or NULL if not found.
static const char *extract(const char *p, const char *limit, const char *open, const char *close,
                           char *out, size_t size) {
	const char *start = strstr(p, open);
	if (!start || (limit && start >= limit))
		return NULL;
	start += strlen(open);
	const char *end = strstr(start, close);
	if (!end || (limit && end > limit))
		return NULL;
	psvt_html_text(out, size, start, (size_t)(end - start));
	return end + strlen(close);
}

static int parse_count(const char *s) {
	// "89,148" or "1.234" -> 89148 / 1234
	int v = 0, any = 0;
	for (; *s; s++) {
		if (isdigit((unsigned char)*s)) {
			v = v * 10 + (*s - '0');
			any = 1;
		} else if (*s != ',' && *s != '.') {
			break;
		}
	}
	return any ? v : -1;
}

static void copy_hash(char *out, const char *src) {
	size_t n = 0;
	while (n < 40 && isxdigit((unsigned char)src[n])) {
		out[n] = (char)tolower((unsigned char)src[n]);
		n++;
	}
	out[n == 40 ? 40 : 0] = '\0';
}

// ---------------------------------------------------------------------------
// List page (index.php?m=list)

int psvt_parse_list(const char *html, PsvtPage *page) {
	memset(page, 0, sizeof(*page));
	page->total_pages = 1;

	const char *total = strstr(html, "<span class=\"small ms-1 text-nowrap\">of ");
	if (total)
		page->total_pages = atoi(total + strlen("<span class=\"small ms-1 text-nowrap\">of "));

	static const char item_marker[] = "<a href=\"theme.php?id=";
	const char *p = html;
	while (page->count < PSVT_MAX_PAGE_ITEMS && (p = strstr(p, item_marker))) {
		p += sizeof(item_marker) - 1;
		const char *end = strstr(p, "</a>");
		if (!end)
			break;
		// Only list-view rows; other links to theme.php are skipped.
		const char *tag_end = strchr(p, '>');
		if (!tag_end || tag_end > end) {
			p = end;
			continue;
		}
		const char *cls = strstr(p, "list-group-item-action");
		if (!cls || cls > tag_end) {
			p = end;
			continue;
		}

		PsvtTheme *t = &page->items[page->count];
		memset(t, 0, sizeof(*t));
		t->id = (uint32_t)strtoul(p, NULL, 10);
		t->downloads = -1;

		const char *img = strstr(p, "/themes/");
		if (img && img < end)
			copy_hash(t->hash, img + strlen("/themes/"));

		extract(p, end, "<h5", "</h5>", t->title, sizeof(t->title));
		// <h5 class="..."> -- strip the remainder of the opening tag
		char *gt = strchr(t->title, '>');
		if (gt)
			memmove(t->title, gt + 1, strlen(gt + 1) + 1);

		extract(p, end, "<p class=\"mb-1 text-muted small\">", "</p>", t->subtitle, sizeof(t->subtitle));
		t->valid = strstr(p, "100% Valid") && strstr(p, "100% Valid") < end;

		char dl[32];
		if (extract(p, end, "<span class=\"badge bg-secondary rounded-pill fs-6\">", "<", dl, sizeof(dl)))
			t->downloads = parse_count(dl);

		if (t->id && t->hash[0] && t->title[0])
			page->count++;
		p = end;
	}

	if (page->count == 0 && !strstr(html, "pagination") && !strstr(html, "list-group"))
		return -1;
	return page->count;
}

// ---------------------------------------------------------------------------
// Detail page (theme.php?id=...)

static void field(const char *html, const char *label, char *out, size_t size) {
	char open[64];
	snprintf(open, sizeof(open), "<strong>%s</strong>", label);
	if (!extract(html, NULL, open, "</li>", out, size))
		out[0] = '\0';
}

int psvt_parse_detail(const char *html, PsvtDetail *d) {
	memset(d, 0, sizeof(*d));
	d->downloads = -1;

	const char *limit = strstr(html, PSVT_DETAIL_END_MARKER);
	(void)limit;

	field(html, "Title:", d->title, sizeof(d->title));
	field(html, "Version:", d->version, sizeof(d->version));
	field(html, "Author (provider):", d->author, sizeof(d->author));
	field(html, "Date added:", d->added, sizeof(d->added));
	field(html, "Last updated:", d->updated, sizeof(d->updated));

	char buf[128];
	field(html, "Downloads:", buf, sizeof(buf));
	d->downloads = parse_count(buf);

	field(html, "Validation score:", buf, sizeof(buf));
	// "100,00% (101 valids, 0 warnings, 0 errors)"
	sscanf(buf, "%15s (%d valids, %d warnings, %d errors)", d->score, &d->valids, &d->warnings,
	       &d->errors);

	const char *dl = strstr(html, "download.php?id=");
	if (dl)
		copy_hash(d->hash, dl + strlen("download.php?id="));

	static const char tag_marker[] = "href=\"./index.php?t=";
	const char *p = html;
	while (d->tag_count < PSVT_MAX_TAGS && (p = strstr(p, tag_marker))) {
		if (limit && p > limit)
			break;
		const char *gt = strchr(p, '>');
		const char *end = gt ? strstr(gt, "</a>") : NULL;
		if (!end)
			break;
		psvt_html_text(d->tags[d->tag_count], sizeof(d->tags[0]), gt + 1, (size_t)(end - gt - 1));
		if (d->tags[d->tag_count][0])
			d->tag_count++;
		p = end;
	}

	return d->title[0] && d->hash[0] ? 0 : -1;
}
