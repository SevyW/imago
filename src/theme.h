// Theme folder metadata: theme.xml and Imago's own marker file.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
	char title[128];
	char provider[64];
	char content_ver[16];
	char package_image[128]; // m_packageImageFilePath (relative)
	char home_preview[128];  // m_homePreviewFilePath
	char start_preview[128]; // m_startPreviewFilePath
} ThemeXml;

// Returns 0 when the document looks like a theme (has a title).
int theme_xml_parse(const char *xml, ThemeXml *out);
int theme_xml_load(const char *dir, ThemeXml *out);
// "01.00" -> 100, the encoding tbl_theme.contentVer uses.
int theme_content_version(const char *ver);

// Written next to theme.xml so Imago can link a folder back to psvt.ovh.
#define THEME_MARKER_FILE "imago.txt"
typedef struct {
	uint32_t id;
	char hash[41];
} ThemeMarker;

int theme_marker_read(const char *dir, ThemeMarker *marker);
int theme_marker_write(const char *dir, const ThemeMarker *marker);

// File-system-safe folder name derived from a title ("Zelda: BotW" -> "Zelda_BotW").
void theme_folder_name(char *out, size_t size, const char *title);

// 1 only for "<ux0|ur0|uma0>:customtheme/<name>" with a single, ordinary
// folder name. Imago never deletes a theme folder that fails this check.
int theme_dir_is_safe(const char *dir);
