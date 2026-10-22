// Application shell shared by the screens: tabs, notifications and the
// installed-theme library. Main thread only.
#pragma once

#include "input.h"
#include "library.h"
#include "psvt.h"
#include "ui.h"

#define APP_VERSION "1.2.0"
#define APP_AUTHOR "Sevy (@SevyW)"

typedef enum { TAB_DISCOVER, TAB_LIBRARY, TAB_ASSETS, TAB_SETTINGS, TAB_COUNT } Tab;

extern Library g_library;
extern int g_library_loading;
extern int g_library_generation; // bumped whenever g_library is replaced
extern int g_db_writable; // unsafe homebrew enabled

void app_toast(const char *text);
void app_format_count(char *out, size_t size, int value); // 89148 -> "89,148"
void app_refresh_library(void);
void app_switch_tab(Tab tab);

// Footer prompts, drawn right to left.
typedef struct {
	Glyph glyph;
	const char *label;
} Hint;
void app_footer(const Hint *hints, int count, const char *status);

// Layout shared by the screens.
#define HEADER_H 64.0f
#define FOOTER_Y 510.0f
#define MARGIN 28.0f

// Screens
void discover_init(void);
void discover_update(const Input *in);
void discover_draw(void);
// True once the first page of themes has arrived (or failed to).
int discover_ready(void);
void discover_set_filter(const char *tag, const char *author);
const PsvtTheme *discover_find(uint32_t id);

void detail_open(const PsvtTheme *theme);
int detail_is_open(void);
void detail_update(const Input *in);
void detail_draw(void);

void library_screen_update(const Input *in);
void library_screen_draw(void);

// Theme Studio (customise a theme and save it as a new one)
int studio_open(const char *dir);
// Opens an already-built theme (Studio takes ownership of *doc). New themes
// always get fresh previews.
int studio_open_doc(void *doc, const char *name, int is_new);
int studio_is_open(void);
void studio_update(const Input *in);
void studio_draw(void);

// Assets (wallpapers, icons, music and lock screens of installed themes)
void assets_screen_update(const Input *in);
void assets_screen_draw(void);
void assets_screen_leave(void);

void settings_update(const Input *in);
// When "Theme wallpapers in Photos" is on, adds what isn't shared yet (in the
// background). announce: report the result even when nothing was added.
void settings_share_photos(int announce);
void settings_draw(void);

// Shared drawing helper: install/update button state for a repository theme.
typedef struct {
	const char *label;
	uint32_t background;
	float progress; // < 0 when idle
	int busy;
} InstallButton;
InstallButton app_install_button(const PsvtTheme *theme);
