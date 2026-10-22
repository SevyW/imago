// Application shell shared by the screens: tabs, notifications and the
// installed-theme library. Main thread only.
#pragma once

#include "apps.h"
#include "input.h"
#include "jobs.h"
#include "library.h"
#include "psvt.h"
#include "ui.h"

#define APP_VERSION "1.2.0"
#define APP_AUTHOR "Sevy (@SevyW)"

typedef enum { TAB_DISCOVER, TAB_LIBRARY, TAB_ASSETS, TAB_APPS, TAB_SETTINGS, TAB_COUNT } Tab;

extern Library g_library;
extern int g_library_loading;
extern int g_library_generation; // bumped whenever g_library is replaced
extern int g_db_writable; // unsafe homebrew enabled

// Installed apps whose artwork can be restyled (the Apps tab).
extern AppList g_apps;
extern int g_apps_loading;
extern int g_apps_generation; // bumped whenever g_apps is replaced

void app_toast(const char *text);
void app_format_count(char *out, size_t size, int value); // 89148 -> "89,148"
void app_refresh_library(void);
void app_switch_tab(Tab tab);
void app_refresh_apps(void);
// Runs work on a background thread behind a "busy" overlay that blocks input
// and the PS button (leaving mid-write could leave an app's artwork half
// replaced); done runs on the main thread afterwards and owns ctx.
void app_run(const char *label, JobWork work, JobFn done, void *ctx);
int app_busy(void);
// Drops cached textures of an app's artwork after it was rewritten.
void app_forget_artwork(const char *id);
// Explains how to enable unsafe homebrew. Returns 1 when it had to.
int app_require_unsafe(void);
// After an artwork change: LiveArea changes show right away, but the home
// screen keeps bubble icons in memory until a restart, so offer one then.
void app_changed(int icon_changed, const char *done);

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

// Apps: restyle installed apps' bubble icons, LiveArea backgrounds and start gates
void apps_screen_update(const Input *in);
void apps_screen_draw(void);
// Puts the custom look back on every app that lost it.
void apps_reapply_all(void);
// Undoes every app's customization.
void apps_restore_all(void);

void appdetail_open(const char *id);
int appdetail_is_open(void);
void appdetail_update(const Input *in);
void appdetail_draw(void);

// Framing a picture for a part, with a live preview of the result.
void editor_open(const char *id, ArtPart part, const char *source);
int editor_is_open(void);
void editor_update(const Input *in);
void editor_draw(void);

// Assets (wallpapers, icons, music and lock screens of installed themes)
void assets_screen_update(const Input *in);
void assets_screen_draw(void);
void assets_screen_leave(void);

// Searching artwork sites for an app's part; a pick opens the editor.
void online_open(const char *id, const char *title, ArtPart part);
int online_is_open(void);
void online_update(const Input *in);
void online_draw(void);

// Settings > Artwork sources: the sites, their keys, connection tests.
void sources_open(void);
int sources_is_open(void);
void sources_update(const Input *in);
void sources_draw(void);

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
