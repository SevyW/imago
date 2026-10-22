#include "livearea.h"

#include "fs.h"
#include "psvt.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int livearea_clean_name(const char *name, size_t len, int trim, char *out, size_t size) {
	if (trim) {
		while (len && isspace((unsigned char)*name))
			name++, len--;
		while (len && isspace((unsigned char)name[len - 1]))
			len--;
	}
	if (len == 0 || len >= size || len >= LIVEAREA_NAME_SIZE)
		return -1;
	for (size_t i = 0; i < len; i++) {
		char c = name[i] == '\\' ? '/' : name[i];
		int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
		         c == '.' || c == ' ' || c == '/';
		if (!ok)
			return -1;
		out[i] = c;
	}
	out[len] = '\0';
	if (out[0] == '/' || strstr(out, "..") || strstr(out, "//"))
		return -1;
	return 0;
}

// Copies the value of attribute `name` from the tag that starts at `tag`.
static void get_attr(const char *tag, const char *end, const char *name, char *out, size_t size) {
	out[0] = '\0';
	size_t n = strlen(name);
	for (const char *p = tag; p + n < end; p++) {
		if (strncmp(p, name, n) != 0 || !isspace((unsigned char)p[-1]))
			continue;
		const char *q = p + n;
		while (q < end && isspace((unsigned char)*q))
			q++;
		if (q >= end || *q != '=')
			continue;
		q++;
		while (q < end && isspace((unsigned char)*q))
			q++;
		char quote = *q;
		if (quote != '"' && quote != '\'')
			continue;
		const char *close = memchr(q + 1, quote, (size_t)(end - q - 1));
		if (close)
			snprintf(out, size, "%.*s", (int)(close - q - 1), q + 1);
		return;
	}
}

// Finds the next <name ...> or <name> tag at or after p, before limit.
static const char *find_tag(const char *p, const char *limit, const char *name) {
	size_t n = strlen(name);
	for (; p && p < limit && (p = strchr(p, '<')) != NULL && p < limit; p++) {
		if (strncmp(p + 1, name, n) == 0 && (p[1 + n] == '>' || isspace((unsigned char)p[1 + n]) || p[1 + n] == '/'))
			return p;
	}
	return NULL;
}

// Collects the file named by each <tag> inside [from, to). The name is the
// element's own text before any child, as in retail templates:
// <image default="on">backgrounds\bg0.png<cntry lang="en">all</cntry></image>
static void collect(const char *from, const char *to, const char *tag, char list[][LIVEAREA_NAME_SIZE], int *count) {
	for (const char *p = from; (p = find_tag(p, to, tag)) != NULL; p++) {
		const char *open_end = strchr(p, '>');
		if (!open_end || open_end >= to || open_end[-1] == '/')
			continue;
		const char *text = open_end + 1, *stop = strchr(text, '<');
		if (!stop || stop > to)
			continue;
		char unescaped[LIVEAREA_NAME_SIZE * 2], name[LIVEAREA_NAME_SIZE];
		// psvt_html_text collapses whitespace, so read leading/trailing spaces as padding.
		psvt_html_text(unescaped, sizeof(unescaped), text, (size_t)(stop - text));
		if (*count >= LIVEAREA_MAX_FILES ||
		    livearea_clean_name(unescaped, strlen(unescaped), 1, name, sizeof(name)) != 0)
			continue;
		int seen = 0;
		for (int i = 0; i < *count; i++)
			seen |= strcmp(list[i], name) == 0;
		if (!seen)
			snprintf(list[(*count)++], LIVEAREA_NAME_SIZE, "%s", name);
	}
}

// The extent of the first <name>...</name> block inside [from, to).
static int block(const char *from, const char *to, const char *name, const char **start, const char **end) {
	const char *open = find_tag(from, to, name);
	if (!open)
		return -1;
	char close[48];
	snprintf(close, sizeof(close), "</%s", name);
	const char *c = strstr(open, close);
	*start = open;
	*end = c && c < to ? c : to;
	return 0;
}

int livearea_parse(const char *xml, LiveArea *out) {
	memset(out, 0, sizeof(*out));
	const char *end = xml + strlen(xml);
	const char *root = find_tag(xml, end, "livearea");
	if (!root)
		return -1;
	const char *root_end = strchr(root, '>');
	if (!root_end)
		return -1;
	get_attr(root, root_end, "style", out->style, sizeof(out->style));
	const char *s, *e;
	if (block(root, end, "livearea-background", &s, &e) == 0)
		collect(s, e, "image", out->bg, &out->bg_count);
	if (block(root, end, "gate", &s, &e) == 0)
		collect(s, e, "startup-image", out->gate, &out->gate_count);
	for (const char *p = root; (p = find_tag(p, end, "frame")) != NULL; p++)
		out->frame_count++;
	return 0;
}

int livearea_read(const char *template_path, LiveArea *out) {
	char *xml;
	memset(out, 0, sizeof(*out));
	if (fs_read_file(template_path, &xml, NULL) != 0)
		return -1;
	int rc = livearea_parse(xml, out);
	free(xml);
	return rc;
}

// Measured from LiveArea screenshots on a console (the gate frame sits 5 px
// outside a 280x158 gate image; the background is drawn at (60, 44)).
static const struct {
	const char *style;
	int x, y;
} gate_origins[] = {
	{"a1", 280, 139},       {"a2", 280, 105}, {"a3", 280, 105},
	{"a4", 280, 105},       {"a5", 520, 105}, {"psmobile", 520, 155},
};

int livearea_gate_origin(const char *style, int *x, int *y) {
	for (size_t i = 0; i < sizeof(gate_origins) / sizeof(gate_origins[0]); i++) {
		if (strcmp(style, gate_origins[i].style) == 0) {
			*x = gate_origins[i].x;
			*y = gate_origins[i].y;
			return 0;
		}
	}
	return -1;
}
