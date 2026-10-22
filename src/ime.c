#include "ime.h"

#ifdef IMAGO_AUTOPILOT
#include "input.h"
#include <stdio.h>
#endif

#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <stdint.h>
#include <string.h>

#define MAX_CHARS 128

static uint16_t title_utf16[64];
static uint16_t initial_utf16[MAX_CHARS + 1];
static uint16_t input_utf16[MAX_CHARS + 1];
static int active;
#ifdef IMAGO_AUTOPILOT
static int scripted; // a TYPE step answers instead of the system keyboard
#endif

static void utf8_to_utf16(const char *src, uint16_t *dst, size_t max) {
	size_t n = 0;
	const unsigned char *s = (const unsigned char *)src;
	while (*s && n + 1 < max) {
		uint32_t c = *s++;
		if (c >= 0xE0 && s[0] && s[1]) {
			c = ((c & 0x0F) << 12) | ((uint32_t)(s[0] & 0x3F) << 6) | (s[1] & 0x3F);
			s += 2;
		} else if (c >= 0xC0 && s[0]) {
			c = ((c & 0x1F) << 6) | (s[0] & 0x3F);
			s += 1;
		}
		dst[n++] = (uint16_t)c;
	}
	dst[n] = 0;
}

static void utf16_to_utf8(const uint16_t *src, char *dst, size_t max) {
	size_t n = 0;
	for (; *src; src++) {
		uint32_t c = *src;
		if (c < 0x80 && n + 1 < max) {
			dst[n++] = (char)c;
		} else if (c < 0x800 && n + 2 < max) {
			dst[n++] = (char)(0xC0 | (c >> 6));
			dst[n++] = (char)(0x80 | (c & 0x3F));
		} else if (n + 3 < max) {
			dst[n++] = (char)(0xE0 | (c >> 12));
			dst[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
			dst[n++] = (char)(0x80 | (c & 0x3F));
		} else {
			break;
		}
	}
	dst[n] = '\0';
}

static int open_dialog(const char *title, const char *initial, int max_length, int code) {
	if (active)
		return -1;
	utf8_to_utf16(title, title_utf16, 64);
	utf8_to_utf16(initial, initial_utf16, MAX_CHARS + 1);
	memset(input_utf16, 0, sizeof(input_utf16));
#ifdef IMAGO_AUTOPILOT
	if (input_autopilot_will_type()) {
		scripted = active = 1;
		return 0;
	}
#endif

	SceImeDialogParam param;
	sceImeDialogParamInit(&param);
	param.supportedLanguages = 0;
	param.languagesForced = SCE_FALSE;
	param.type = code ? SCE_IME_TYPE_BASIC_LATIN : SCE_IME_TYPE_DEFAULT;
	param.option = code ? SCE_IME_OPTION_NO_AUTO_CAPITALIZATION | SCE_IME_OPTION_NO_ASSISTANCE : 0;
	param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
	param.title = title_utf16;
	param.maxTextLength = (SceUInt32)(max_length < MAX_CHARS ? max_length : MAX_CHARS);
	param.initialText = initial_utf16;
	param.inputTextBuffer = input_utf16;
	if (sceImeDialogInit(&param) < 0)
		return -1;
	active = 1;
	return 0;
}

int ime_open(const char *title, const char *initial, int max_length) {
	return open_dialog(title, initial, max_length, 0);
}

int ime_open_code(const char *title, const char *initial, int max_length) {
	return open_dialog(title, initial, max_length, 1);
}

int ime_active(void) {
	return active;
}

int ime_poll(char *out, size_t size) {
	if (!active)
		return -1;
#ifdef IMAGO_AUTOPILOT
	if (scripted) { // a scripted answer, for recordings
		const char *typed = input_autopilot_text();
		if (!typed)
			return 0;
		scripted = active = 0;
		snprintf(out, size, "%s", typed);
		return 1;
	}
#endif
	if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
		return 0;
	SceImeDialogResult result;
	memset(&result, 0, sizeof(result));
	sceImeDialogGetResult(&result);
	sceImeDialogTerm();
	active = 0;
	if (result.button != SCE_IME_DIALOG_BUTTON_ENTER)
		return -1;
	utf16_to_utf8(input_utf16, out, size);
	return 1;
}
