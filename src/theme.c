#include "theme.h"

#include "fs.h"
#include "psvt.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Copies the text of the first <tag>...</tag> found between start and limit.
static int xml_value(const char *start, const char *limit, const char *tag, char *out, size_t size) {
	char open[64], close[64];
	snprintf(open, sizeof(open), "<%s>", tag);
	snprintf(close, sizeof(close), "</%s>", tag);
	const char *a = strstr(start, open);
	if (!a || (limit && a >= limit))
		return -1;
	a += strlen(open);
	const char *b = strstr(a, close);
	if (!b || (limit && b > limit))
		return -1;
	psvt_html_text(out, size, a, (size_t)(b - a));
	return 0;
}

// Reads <m_default> inside a localized block such as <m_title>.
static void localized(const char *xml, const char *limit, const char *tag, char *out, size_t size) {
	char open[64], close[64];
	snprintf(open, sizeof(open), "<%s>", tag);
	snprintf(close, sizeof(close), "</%s>", tag);
	const char *a = strstr(xml, open);
	const char *b = a ? strstr(a, close) : NULL;
	if (!a || !b || (limit && b > limit) || xml_value(a, b, "m_default", out, size) != 0)
		out[0] = '\0';
}

int theme_xml_parse(const char *xml, ThemeXml *out) {
	memset(out, 0, sizeof(*out));
	const char *info = strstr(xml, "<InfomationProperty>");
	const char *limit = info ? strstr(info, "</InfomationProperty>") : NULL;
	if (!info)
		info = xml;
	localized(info, limit, "m_title", out->title, sizeof(out->title));
	localized(info, limit, "m_provider", out->provider, sizeof(out->provider));
	xml_value(info, limit, "m_contentVer", out->content_ver, sizeof(out->content_ver));
	xml_value(info, limit, "m_packageImageFilePath", out->package_image, sizeof(out->package_image));
	xml_value(info, limit, "m_homePreviewFilePath", out->home_preview, sizeof(out->home_preview));
	xml_value(info, limit, "m_startPreviewFilePath", out->start_preview, sizeof(out->start_preview));
	return out->title[0] ? 0 : -1;
}

int theme_xml_load(const char *dir, ThemeXml *out) {
	char path[512];
	snprintf(path, sizeof(path), "%s/theme.xml", dir);
	char *xml;
	if (fs_read_file(path, &xml, NULL) != 0) {
		memset(out, 0, sizeof(*out));
		return -1;
	}
	int rc = theme_xml_parse(xml, out);
	free(xml);
	return rc;
}

int theme_content_version(const char *ver) {
	int major = 0, minor = 0;
	if (sscanf(ver, "%d.%d", &major, &minor) < 1)
		return 100;
	return major * 100 + minor;
}

int theme_marker_read(const char *dir, ThemeMarker *marker) {
	memset(marker, 0, sizeof(*marker));
	char path[512];
	snprintf(path, sizeof(path), "%s/" THEME_MARKER_FILE, dir);
	char *text;
	if (fs_read_file(path, &text, NULL) != 0)
		return -1;
	const char *id = strstr(text, "id=");
	const char *hash = strstr(text, "hash=");
	if (id)
		marker->id = (uint32_t)strtoul(id + 3, NULL, 10);
	if (hash) {
		size_t n = 0;
		hash += 5;
		while (n < 40 && isxdigit((unsigned char)hash[n])) {
			marker->hash[n] = hash[n];
			n++;
		}
		marker->hash[n] = '\0';
	}
	free(text);
	return marker->id ? 0 : -1;
}

int theme_marker_write(const char *dir, const ThemeMarker *marker) {
	char path[512], text[128];
	snprintf(path, sizeof(path), "%s/" THEME_MARKER_FILE, dir);
	int n = snprintf(text, sizeof(text), "id=%u\nhash=%s\n", (unsigned)marker->id, marker->hash);
	return fs_write_file(path, text, (size_t)n);
}

void theme_folder_name(char *out, size_t size, const char *title) {
	size_t n = 0;
	int underscore = 1; // suppress leading and repeated separators
	for (const unsigned char *p = (const unsigned char *)title; *p && n + 1 < size && n < 48; p++) {
		if (isalnum(*p) || *p == '-' || *p == '.') {
			out[n++] = (char)*p;
			underscore = 0;
		} else if (!underscore) {
			out[n++] = '_';
			underscore = 1;
		}
	}
	while (n > 0 && (out[n - 1] == '_' || out[n - 1] == '.'))
		n--;
	out[n] = '\0';
	if (n == 0)
		snprintf(out, size, "theme");
}

int theme_dir_is_safe(const char *dir) {
	static const char *const roots[] = {"ux0:customtheme/", "ur0:customtheme/", "uma0:customtheme/"};
	const char *name = NULL;
	for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
		if (strncmp(dir, roots[i], strlen(roots[i])) == 0)
			name = dir + strlen(roots[i]);
	if (!name || !name[0] || strchr(name, '/') || strchr(name, '\\') || strchr(name, ':'))
		return 0;
	return strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}
