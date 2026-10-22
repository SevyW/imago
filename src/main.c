// Imago: a fast custom theme browser for the PS Vita.
#include "app.h"
#include "config.h"
#include "dialog.h"
#include "folders.h"
#include "fs.h"
#include "images.h"
#include "ime.h"
#include "jobs.h"
#include "keys.h"
#include "log.h"
#include "music.h"
#include "net.h"
#include "appdb.h"
#include "livearea.h"
#include "picture_picker.h"
#include "store.h"
#include "tasks.h"
#include "themedb.h"

#include <math.h>
#include <psp2/apputil.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/common_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/shellutil.h>
#include <psp2/system_param.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int _newlib_heap_size_user = 96 * 1024 * 1024;

Library g_library;
int g_library_loading;
int g_library_generation;
int g_db_writable;

static Tab tab = TAB_DISCOVER;
static float tab_x, tab_w; // animated underline
static float tab_label_x[TAB_COUNT], tab_label_w[TAB_COUNT];
static int library_refresh_queued;

AppList g_apps;
int g_apps_loading;
int g_apps_generation;
static int apps_refresh_queued;

static char busy_label[64];
static int busy;

#define TOAST_SECONDS 3.2f
#define MAX_TOASTS 4
static char toasts[MAX_TOASTS][160];
static int toast_count;
static float toast_age;

// --- shared services --------------------------------------------------------------

void app_toast(const char *text) {
	log_write("notice: %s", text);
	if (toast_count == MAX_TOASTS) { // drop the oldest
		memmove(toasts[0], toasts[1], sizeof(toasts[0]) * (MAX_TOASTS - 1));
		toast_count--;
	}
	snprintf(toasts[toast_count++], sizeof(toasts[0]), "%s", text);
	if (toast_count == 1)
		toast_age = 0.0f;
}

void app_format_count(char *out, size_t size, int value) {
	if (value < 0) {
		snprintf(out, size, "-");
		return;
	}
	char digits[16];
	int n = snprintf(digits, sizeof(digits), "%d", value);
	size_t o = 0;
	for (int i = 0; i < n && o + 2 < size; i++) {
		if (i > 0 && (n - i) % 3 == 0)
			out[o++] = ',';
		out[o++] = digits[i];
	}
	out[o] = '\0';
}

static void library_work(void *ctx, NetClient *net) {
	(void)net;
	library_scan((Library *)ctx);
}

static void library_done(void *ctx) {
	library_free(&g_library);
	g_library = *(Library *)ctx;
	free(ctx);
	g_library_generation++;
	g_library_loading = 0;
	int local = 0;
	for (int i = 0; i < g_library.count; i++)
		local += !g_library.items[i].registered;
	settings_share_photos(0); // new or saved themes' wallpapers, if that's on
	if (g_library.db_ok)
		log_write("library: %d themes (%d not added)", g_library.count, local);
	else
		log_write("library: %d theme folders; theme list unreadable: %s", g_library.count, g_library.db_error);
	if (library_refresh_queued) {
		library_refresh_queued = 0;
		app_refresh_library();
	}
}

void app_refresh_library(void) {
	if (g_library_loading) {
		library_refresh_queued = 1; // coalesce bursts (e.g. batch installs)
		return;
	}
	Library *lib = calloc(1, sizeof(*lib));
	if (!lib)
		return;
	g_library_loading = 1;
	jobs_submit(library_work, library_done, lib);
}

// --- the Apps tab's services ---------------------------------------------------------

static void apps_work(void *ctx, NetClient *net) {
	(void)net;
	apps_scan((AppList *)ctx);
}

static void apps_done(void *ctx) {
	apps_free(&g_apps);
	g_apps = *(AppList *)ctx;
	free(ctx);
	g_apps_generation++;
	g_apps_loading = 0;
	log_write("apps: %d apps", g_apps.count);
	if (apps_refresh_queued) {
		apps_refresh_queued = 0;
		app_refresh_apps();
	}
}

void app_refresh_apps(void) {
	if (g_apps_loading) {
		apps_refresh_queued = 1;
		return;
	}
	AppList *list = calloc(1, sizeof(*list));
	if (!list)
		return;
	g_apps_loading = 1;
	jobs_submit(apps_work, apps_done, list);
}

typedef struct {
	JobWork work;
	JobFn done;
	void *ctx;
} RunCtx;

static void run_work(void *ctx, NetClient *net) {
	RunCtx *r = ctx;
	r->work(r->ctx, net);
}

static void run_done(void *ctx) {
	RunCtx *r = ctx;
	busy = 0;
	sceShellUtilUnlock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN);
	if (r->done)
		r->done(r->ctx);
	free(r);
}

void app_run(const char *label, JobWork work, JobFn done, void *ctx) {
	RunCtx *r = malloc(sizeof(*r));
	if (!r) {
		if (done)
			done(ctx);
		return;
	}
	*r = (RunCtx){work, done, ctx};
	snprintf(busy_label, sizeof(busy_label), "%s", label);
	busy = 1;
	log_write("apps: %s", label);
	// Leaving mid-write could leave an app's artwork half replaced.
	sceShellUtilLock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN);
	jobs_submit(run_work, run_done, r);
}

int app_busy(void) {
	return busy;
}

void app_forget_artwork(const char *id) {
	char path[512];
	store_appmeta_path(id, "icon0.png", path, sizeof(path));
	images_forget(path);
	for (int p = 0; p < ART_COUNT; p++) {
		store_custom_path(id, (ArtPart)p, path, sizeof(path));
		images_forget(path);
	}
	LiveArea la;
	if (appdb_livearea(id, &la) != 0)
		return;
	for (int i = 0; i < la.bg_count + la.gate_count; i++) {
		char rel[LIVEAREA_NAME_SIZE + 32];
		snprintf(rel, sizeof(rel), "livearea/contents/%s", i < la.bg_count ? la.bg[i] : la.gate[i - la.bg_count]);
		store_appmeta_path(id, rel, path, sizeof(path));
		images_forget(path);
	}
}

int app_require_unsafe(void) {
	if (g_db_writable)
		return 0;
	dialog_message("Unsafe homebrew is disabled",
	               "Changing an app's artwork needs write access to the system's artwork folder. Enable "
	               "\"Unsafe homebrew\" in HENkaku Settings, then restart Imago.");
	return 1;
}

static void on_restart_for_icon(int choice) {
	if (choice == 0)
		scePowerRequestColdReset();
}

void app_changed(int icon_changed, const char *done) {
	char text[200];
	if (!icon_changed) {
		snprintf(text, sizeof(text), "%s. Open the app's LiveArea to see it.", done);
		app_toast(text);
		return;
	}
	snprintf(text, sizeof(text),
	         "%s. LiveArea changes show right away, but the home screen only picks up a new bubble icon "
	         "after a restart.",
	         done);
	dialog_confirm("Restart to see the icon?", text, "Restart now", 0, on_restart_for_icon);
}

// Pictura became Imago's Apps tab: its backups and customizations move over
// once (both folders are on ux0:, so it's a rename).
static void migrate_pictura(void) {
	if (fs_exists(STORE_DIR) || !fs_is_dir("ux0:data/Pictura/apps"))
		return;
	if (fs_rename("ux0:data/Pictura/apps", STORE_DIR) == 0)
		log_write("apps: moved Pictura's app backups to " STORE_DIR);
	else
		log_write("apps: couldn't move ux0:data/Pictura/apps");
}

static void draw_busy(void) {
	if (!busy)
		return;
	ui_rect(0, 0, SCREEN_W, SCREEN_H, C_SCRIM);
	float w = (float)ui_text_width(FONT_BODY, busy_label) + 80.0f, h = 64.0f;
	float x = (SCREEN_W - w) * 0.5f, y = (SCREEN_H - h) * 0.5f;
	ui_shadow(x, y, w, h, 18.0f, 1.0f);
	ui_rrect(x, y, w, h, 18.0f, C_SURFACE2);
	ui_spinner(x + 32.0f, y + h * 0.5f, 10.0f, C_ACCENT);
	ui_text(FONT_BODY, x + 56.0f, y + h * 0.5f + 6.0f, C_TEXT, busy_label);
}

void app_switch_tab(Tab t) {
	tab = t;
}

InstallButton app_install_button(const PsvtTheme *theme) {
	static char label[48];
	InstallButton b = {"Install", C_ACCENT, -1.0f, 0};
	Task task;
	if (tasks_find(theme->id, &task) && task.state <= TASK_FINISHING) {
		int pct = (int)(task.progress * 100.0f);
		b.busy = 1;
		b.progress = task.progress;
		switch (task.state) {
		case TASK_QUEUED: snprintf(label, sizeof(label), "Queued"); break;
		case TASK_DOWNLOADING: snprintf(label, sizeof(label), "Downloading %d%%", pct); break;
		case TASK_EXTRACTING: snprintf(label, sizeof(label), "Installing %d%%", pct); break;
		default: snprintf(label, sizeof(label), "Finishing..."); break;
		}
		b.label = label;
		return b;
	}
	switch (library_status(&g_library, theme->id, theme->hash, theme->title)) {
	case THEME_UPDATE:
		b.label = "Update";
		b.background = RGBA8(201, 138, 18, 255);
		break;
	case THEME_INSTALLED:
		b.label = "Reinstall";
		b.background = C_SURFACE3;
		break;
	default:
		break;
	}
	return b;
}

void app_footer(const Hint *hints, int count, const char *status) {
	float cy = FOOTER_Y + 17.0f;
	float x = SCREEN_W - MARGIN;
	for (int i = 0; i < count; i++)
		x = ui_hint(x, cy, hints[i].glyph, hints[i].label);
	if (status)
		ui_text(FONT_SMALL, MARGIN, cy + 4.5f, C_TEXT3, status);
}

// --- task notifications -------------------------------------------------------------

static void report_finished_tasks(void) {
	Task t;
	while (tasks_pop_finished(&t)) {
		char text[200];
		if (t.state == TASK_FAILED) {
			snprintf(text, sizeof(text), "\"%s\": %s", t.title, t.message);
			if (!dialog_open())
				dialog_message("Something went wrong", text);
			else
				app_toast(text);
		} else if (t.state == TASK_CANCELED) {
			app_toast("Installation canceled");
		} else if (t.kind == TASK_INSTALL) {
			snprintf(text, sizeof(text), "\"%s\" installed. Apply it in Settings > Theme & Background.", t.title);
			app_toast(text);
		} else if (t.kind == TASK_BUILD) {
			snprintf(text, sizeof(text), "\"%s\" saved. Apply it in Settings > Theme & Background.", t.title);
			app_toast(text);
		} else if (t.kind == TASK_REGISTER) {
			snprintf(text, sizeof(text), "\"%s\" added to your themes", t.title);
			app_toast(text);
		} else {
			snprintf(text, sizeof(text), "\"%s\" removed", t.title);
			app_toast(text);
		}
		app_refresh_library();
	}
}

// --- chrome -------------------------------------------------------------------------

// The ghost from the app icon, then the IMAGO wordmark with a red triangle for the A.
static void draw_logo(float x, float y) {
	ImageRef logo = images_file("app0:assets/logo.png");
	if (logo.tex)
		ui_image(logo.tex, x, y, 34.0f, 34.0f, 9.0f, 9.0f, 9.0f, 9.0f, ui_alpha(0xFFFFFFFF, logo.alpha));
	else
		ui_rrect(x, y, 34.0f, 34.0f, 9.0f, C_SURFACE2);

	static const char *const letters[] = {"I", "M", NULL, "G", "O"};
	float cx = x + 47.0f, base = y + 24.0f;
	for (int i = 0; i < 5; i++) {
		if (letters[i]) {
			cx += (float)ui_text(FONT_TITLE, cx, base, C_TEXT, letters[i]) + 6.0f;
			continue;
		}
		float w = 14.0f, top = base - 13.5f;
		ui_line(cx, base, cx + w * 0.5f, top, 2.0f, C_ACCENT);
		ui_line(cx + w * 0.5f, top, cx + w, base, 2.0f, C_ACCENT);
		ui_line(cx - 0.6f, base - 0.6f, cx + w + 0.6f, base - 0.6f, 2.0f, C_ACCENT);
		cx += w + 6.0f;
	}
}

static void draw_header(void) {
	static const char *const names[TAB_COUNT] = {"Discover", "Library", "Assets", "Apps", "Settings"};
	draw_logo(MARGIN, 15.0f);

	float spacing = 30.0f, total = 0.0f;
	for (int i = 0; i < TAB_COUNT; i++) {
		tab_label_w[i] = (float)ui_text_width(FONT_BOLD, names[i]);
		total += tab_label_w[i] + (i ? spacing : 0.0f);
	}
	float x = (SCREEN_W - total) * 0.5f;
	ui_glyph(GLYPH_L, x - 26.0f, 33.0f, 0.9f);
	for (int i = 0; i < TAB_COUNT; i++) {
		tab_label_x[i] = x;
		ui_text(FONT_BOLD, x, 39.0f, i == (int)tab ? C_TEXT : C_TEXT3, names[i]);
		x += tab_label_w[i] + spacing;
	}
	ui_glyph(GLYPH_R, x - spacing + 26.0f, 33.0f, 0.9f);

	if (tab_w == 0.0f) {
		tab_x = tab_label_x[tab];
		tab_w = tab_label_w[tab];
	}
	tab_x = ui_approach(tab_x, tab_label_x[tab], 18.0f);
	tab_w = ui_approach(tab_w, tab_label_w[tab], 18.0f);
	ui_rrect(tab_x, 49.0f, tab_w, 3.0f, 1.5f, C_ACCENT);

	// Background work indicator
	Task t;
	int pending = tasks_active(&t);
	if (pending > 0) {
		char label[64];
		int pct = (int)(t.progress * 100.0f);
		if (t.kind != TASK_INSTALL)
			snprintf(label, sizeof(label), "Working...");
		else if (t.state == TASK_QUEUED)
			snprintf(label, sizeof(label), "Queued");
		else if (pending > 1)
			snprintf(label, sizeof(label), "Installing %d%% \xC2\xB7 %d more", pct, pending - 1);
		else
			snprintf(label, sizeof(label), "Installing %d%%", pct);
		float right = SCREEN_W - MARGIN;
		int w = ui_text_width(FONT_SMALL, label);
		ui_rrect(right - (float)w - 40.0f, 19.0f, (float)w + 40.0f, 28.0f, 14.0f, C_SURFACE);
		ui_spinner(right - (float)w - 22.0f, 33.0f, 7.0f, C_ACCENT);
		ui_text(FONT_SMALL, right - (float)w - 10.0f, 37.5f, C_TEXT2, label);
	}
}

static void draw_toast(void) {
	if (toast_count == 0)
		return;
	toast_age += ui_dt();
	if (toast_age > TOAST_SECONDS) {
		memmove(toasts[0], toasts[1], sizeof(toasts[0]) * (MAX_TOASTS - 1));
		toast_count--;
		toast_age = 0.0f;
		if (toast_count == 0)
			return;
	}
	float in = toast_age / 0.18f, out = (TOAST_SECONDS - toast_age) / 0.25f;
	float a = in < out ? in : out;
	a = a > 1.0f ? 1.0f : a;
	char text[160];
	ui_text_fit(FONT_BODY, toasts[0], 760, text, sizeof(text));
	float w = (float)ui_text_width(FONT_BODY, text) + 40.0f, h = 40.0f;
	float x = (SCREEN_W - w) * 0.5f, y = FOOTER_Y - 52.0f + (1.0f - a) * 10.0f;
	ui_shadow(x, y, w, h, 20.0f, a);
	ui_rrect(x, y, w, h, 20.0f, ui_alpha(C_SURFACE3, a));
	ui_text(FONT_BODY, x + 20.0f, y + 26.0f, ui_alpha(C_TEXT, a), text);
}

static const char *const tab_names[TAB_COUNT] = {"Discover", "Library", "Assets", "Apps", "Settings"};

static void update_tabs(const Input *in) {
	if (in->repeat & SCE_CTRL_LTRIGGER)
		tab = (Tab)((tab + TAB_COUNT - 1) % TAB_COUNT);
	if (in->repeat & SCE_CTRL_RTRIGGER)
		tab = (Tab)((tab + 1) % TAB_COUNT);
	if (in->tapped && in->tap_y < HEADER_H) {
		for (int i = 0; i < TAB_COUNT; i++)
			if (in->tap_x >= tab_label_x[i] - 12 && in->tap_x <= tab_label_x[i] + tab_label_w[i] + 12)
				tab = (Tab)i;
	}
}

// --- opening splash ---------------------------------------------------------------

// The key art covers the screen while the first page of themes loads, then
// fades into the app. Any button or tap skips it.
#define SPLASH_MIN 1.4f  // seconds the art stays up at least
#define SPLASH_MAX 3.5f  // ...and at most, even if the themes are still loading
#define SPLASH_LATE 2.0f // give up if the art itself takes this long to decode
#define SPLASH_FADE 0.35f
static int splash_on = 1, splash_leaving;
static float splash_wait, splash_age, splash_fade;

// Returns nonzero while the splash should swallow input.
static int update_splash(const Input *in) {
	if (!splash_on)
		return 0;
	ImageRef art = images_file("app0:assets/splash.jpg");
	if (art.failed) {
		splash_on = 0;
		return 0;
	}
	if (art.tex)
		splash_age += ui_dt();
	else
		splash_wait += ui_dt();
	if (!splash_leaving && (in->pressed || in->tapped || (!art.tex && splash_wait >= SPLASH_LATE) ||
	                        splash_age >= SPLASH_MAX || (splash_age >= SPLASH_MIN && discover_ready())))
		splash_leaving = 1;
	if (splash_leaving) {
		splash_fade += ui_dt() / SPLASH_FADE;
		if (splash_fade >= 1.0f)
			splash_on = 0;
	}
	return 1;
}

static void draw_splash(void) {
	if (!splash_on)
		return;
	float a = 1.0f - (splash_fade > 1.0f ? 1.0f : splash_fade);
	a = a * a * (3.0f - 2.0f * a); // smoothstep
	ui_rect(0, 0, SCREEN_W, SCREEN_H, ui_alpha(C_BG, a));
	ImageRef art = images_file("app0:assets/splash.jpg");
	if (art.tex)
		ui_image(art.tex, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 0, ui_alpha(0xFFFFFFFF, a * art.alpha));
	if (!splash_leaving && splash_age >= SPLASH_MIN)
		ui_spinner(SCREEN_W - 36.0f, SCREEN_H - 36.0f, 9.0f, ui_alpha(C_TEXT2, a));
}

#ifdef IMAGO_AUTOPILOT
#include "imageio.h"
// Test builds: copies the finished frame to a PNG when autopilot.txt asks.
static void save_autopilot_shot(void) {
	unsigned frame = input_autopilot_shot();
	if (!frame)
		return;
	vita2d_wait_rendering_done();
	const uint32_t *fb = vita2d_get_current_fb();
	uint32_t *copy = malloc(SCREEN_W * SCREEN_H * 4);
	if (!fb || !copy) {
		free(copy);
		return;
	}
	for (int y = 0; y < SCREEN_H; y++) // vita2d's framebuffer rows are 1024 pixels
		for (int x = 0; x < SCREEN_W; x++)
			copy[y * SCREEN_W + x] = fb[y * 1024 + x] | 0xFF000000u; // RGBA bytes; force opaque
	char path[96];
	fs_mkdir_p(DATA_DIR "/shots");
	snprintf(path, sizeof(path), DATA_DIR "/shots/%06u.png", frame);
	image_save_png(path, (const uint8_t *)copy, SCREEN_W, SCREEN_H);
	free(copy);
}

// Test builds: while ux0:data/Imago/record exists, saves about 10 frames a
// second (checked every half second) as raw framebuffers named by milliseconds (for the README's clips).
static void record_frame(void) {
	static int recording;
	static unsigned long long start;
	static unsigned count;
	if (count++ % 30 == 0) { // the flag is checked twice a second, so recording can start and stop
		int was = recording;
		recording = fs_exists(DATA_DIR "/record");
		if (recording && !was) {
			fs_mkdir_p(DATA_DIR "/rec");
			start = sceKernelGetProcessTimeWide();
		}
	}
	if (!recording || count % 6)
		return;
	vita2d_wait_rendering_done();
	const uint32_t *fb = vita2d_get_current_fb();
	char path[96];
	snprintf(path, sizeof(path), DATA_DIR "/rec/%06u.raw", (unsigned)((sceKernelGetProcessTimeWide() - start) / 1000));
	FILE *f = fopen(path, "wb");
	if (f) {
		fwrite(fb, 4, 1024 * SCREEN_H, f); // rows are 1024 pixels apart; cropped on the PC
		fclose(f);
	}
}
#endif

// The first lines of every session: enough to know what someone was running.
static void log_session(void) {
	SceKernelSystemSwVersion fw;
	memset(&fw, 0, sizeof(fw));
	fw.size = sizeof(fw);
	if (sceKernelGetSystemSwVersion(&fw) < 0 || !fw.versionString[0])
		snprintf(fw.versionString, sizeof(fw.versionString), "unknown");
	char space[24] = "unknown";
	int64_t free_bytes = fs_free_space("ux0:");
	if (free_bytes >= 0)
		fs_format_size(space, sizeof(space), free_bytes);
	log_write("Imago %s on firmware %s, %s; ux0: %s free; unsafe homebrew %s; installing to %scustomtheme",
	          APP_VERSION, fw.versionString,
	          sceKernelGetModelForCDialog() == SCE_KERNEL_MODEL_VITATV ? "PS TV" : "PS Vita", space,
	          g_db_writable ? "on" : "off", config_partition_device(g_config.partition));

	// Plugins that change the home screen (livearea_nolimits, bubble layout
	// tools...) can interact with themes, so reports should show them. taiHEN
	// uses ux0:tai/config.txt when it exists, ur0:tai/config.txt otherwise.
	const char *config = fs_exists("ux0:tai/config.txt") ? "ux0:tai/config.txt" : "ur0:tai/config.txt";
	char *text;
	if (fs_read_file(config, &text, NULL) != 0) {
		log_write("plugins: no taiHEN config found");
		return;
	}
	char list[900] = "";
	size_t len = 0;
	for (char *line = strtok(text, "\r\n"); line && len + 80 < sizeof(list); line = strtok(NULL, "\r\n")) {
		while (*line == ' ' || *line == '\t')
			line++;
		if (!*line || *line == '#')
			continue;
		if (*line == '*') { // a section: *KERNEL, *main, *ALL or a title id
			char *end = line + strcspn(line, " \t#");
			*end = '\0';
			len += (size_t)snprintf(list + len, sizeof(list) - len, "%s[%s]", len ? " " : "", line + 1);
			continue;
		}
		char *end = line + strcspn(line, " \t#");
		*end = '\0';
		const char *name = strrchr(line, '/');
		name = name ? name + 1 : strchr(line, ':') ? strchr(line, ':') + 1 : line;
		len += (size_t)snprintf(list + len, sizeof(list) - len, " %s", name);
	}
	free(text);
	log_write("plugins (%s):%s%s", config, list[0] == '[' ? " " : "", list);
}

// --- main -------------------------------------------------------------------------

static void init_system(void) {
	SceAppUtilInitParam init;
	SceAppUtilBootParam boot;
	memset(&init, 0, sizeof(init));
	memset(&boot, 0, sizeof(boot));
	sceAppUtilInit(&init, &boot);

	SceCommonDialogConfigParam config;
	sceCommonDialogConfigParamInit(&config);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, (int *)&config.language);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, (int *)&config.enterButtonAssign);
	sceCommonDialogSetConfigParam(&config);
}

int main(void) {
	init_system();
	vita2d_init();
	vita2d_set_clear_color(C_BG);
	if (ui_init() != 0) {
		vita2d_fini();
		sceKernelExitProcess(0);
		return 0;
	}

	fs_mkdir_p(DATA_DIR);
	log_init();
	config_load();
	keys_load();
	folders_load();
	input_init();
	net_init();
	jobs_init(2);
	images_init();
	migrate_pictura();
	tasks_init();
	music_init();
	g_db_writable = themedb_writable();
	log_session();
	app_refresh_library();
	discover_init();

	// For testing over vitacompanion: its synthetic input doesn't count as
	// activity, so the screen would go off (and lock) mid-test.
	int keep_awake = fs_exists(DATA_DIR "/keep-awake");
	if (keep_awake)
		log_write("keep-awake: the screen stays on");

	Input in;
	memset(&in, 0, sizeof(in));
	for (;;) {
		if (keep_awake)
			sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
		input_update(&in);
		ui_begin_frame();
		images_frame();
		jobs_poll();
		report_finished_tasks();

		Input routed = in;
		if (update_splash(&in) || busy)
			memset(&routed, 0, sizeof(routed));

		if (dialog_open()) {
			dialog_update(&routed);
		} else if (picture_picker_is_open()) {
			picture_picker_update(&routed);
		} else if (editor_is_open()) {
			editor_update(&routed);
		} else if (online_is_open()) {
			online_update(&routed);
		} else if (appdetail_is_open()) {
			appdetail_update(&routed);
		} else if (sources_is_open()) {
			sources_update(&routed);
		} else if (studio_is_open()) {
			studio_update(&routed);
		} else if (detail_is_open()) {
			detail_update(&routed);
		} else {
			Tab before = tab;
			if (!ime_active())
				update_tabs(&routed);
			if (before == TAB_ASSETS && tab != TAB_ASSETS)
				assets_screen_leave();
			if (tab != before)
				log_write("tab: %s", tab_names[tab]);
			if (tab == TAB_APPS && g_apps_generation == 0 && !g_apps_loading)
				app_refresh_apps(); // the first visit lists the apps
			switch (tab) {
			case TAB_DISCOVER: discover_update(&routed); break;
			case TAB_LIBRARY: library_screen_update(&routed); break;
			case TAB_ASSETS: assets_screen_update(&routed); break;
			case TAB_APPS: apps_screen_update(&routed); break;
			default: settings_update(&routed); break;
			}
		}

		vita2d_start_drawing();
		vita2d_clear_screen();
		ui_vgradient(0, 0, SCREEN_W, 180.0f, ui_alpha(C_ACCENT, 0.09f), ui_alpha(C_ACCENT, 0.0f));
		draw_header();
		switch (tab) {
		case TAB_DISCOVER: discover_draw(); break;
		case TAB_LIBRARY: library_screen_draw(); break;
		case TAB_ASSETS: assets_screen_draw(); break;
		case TAB_APPS: apps_screen_draw(); break;
		default: settings_draw(); break;
		}
		detail_draw();
		studio_draw();
		sources_draw();
		appdetail_draw();
		online_draw();
		editor_draw();
		picture_picker_draw();
		draw_toast();
		dialog_draw();
		draw_busy();
		draw_splash();
		vita2d_end_drawing();
#ifdef IMAGO_AUTOPILOT
		save_autopilot_shot();
		record_frame();
#endif
		vita2d_common_dialog_update();
		vita2d_swap_buffers();
	}
}
