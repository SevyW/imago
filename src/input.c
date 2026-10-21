#include "input.h"

#include <psp2/apputil.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/system_param.h>
#include <psp2/touch.h>
#include <stdlib.h>
#include <string.h>

#define REPEAT_DELAY_US 340000ULL
#define REPEAT_RATE_US 70000ULL
#define TAP_MAX_US 350000ULL
#define DRAG_THRESHOLD 14
#define REPEATABLE                                                                                 \
	(SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)

uint32_t BTN_OK = SCE_CTRL_CROSS;
uint32_t BTN_BACK = SCE_CTRL_CIRCLE;

#ifdef IMAGO_AUTOPILOT
// Test builds only: replays "<frame> <BUTTON>" lines from autopilot.txt so
// screens can be exercised in an emulator without a focused window.
#include <stdio.h>

#define AUTOPILOT_PATH "ux0:data/Imago/autopilot.txt"
#define MAX_STEPS 256

static struct {
	unsigned frame;
	uint32_t buttons;
} steps[MAX_STEPS];
static int step_count, step_next;
static unsigned frame_no;

static uint32_t button_named(const char *name) {
	static const struct {
		const char *name;
		uint32_t mask;
	} names[] = {
		{"UP", SCE_CTRL_UP},           {"DOWN", SCE_CTRL_DOWN},         {"LEFT", SCE_CTRL_LEFT},
		{"RIGHT", SCE_CTRL_RIGHT},     {"CROSS", SCE_CTRL_CROSS},       {"CIRCLE", SCE_CTRL_CIRCLE},
		{"SQUARE", SCE_CTRL_SQUARE},   {"TRIANGLE", SCE_CTRL_TRIANGLE}, {"L", SCE_CTRL_LTRIGGER},
		{"R", SCE_CTRL_RTRIGGER},      {"START", SCE_CTRL_START},       {"SELECT", SCE_CTRL_SELECT},
	};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (strcmp(names[i].name, name) == 0)
			return names[i].mask;
	return 0;
}

static void autopilot_load(void) {
	FILE *f = fopen(AUTOPILOT_PATH, "r");
	if (!f)
		return;
	char name[32];
	unsigned frame;
	while (step_count < MAX_STEPS && fscanf(f, "%u %31s", &frame, name) == 2) {
		steps[step_count].frame = frame;
		steps[step_count].buttons = button_named(name);
		step_count++;
	}
	fclose(f);
}

static uint32_t autopilot_buttons(void) {
	frame_no++;
	uint32_t held = 0;
	for (int i = step_next; i < step_count; i++) {
		if (steps[i].frame > frame_no)
			break;
		if (frame_no <= steps[i].frame + 2) // hold each press for a few frames
			held |= steps[i].buttons;
		else if (i == step_next)
			step_next++;
	}
	return held;
}
#endif

static uint32_t previous;
static unsigned long long held_since, last_repeat;
static int was_touching, start_x, start_y, last_y;
static unsigned long long touch_start;

void input_init(void) {
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
#ifdef IMAGO_AUTOPILOT
	autopilot_load();
#endif
	int enter = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter);
	if (enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) {
		BTN_OK = SCE_CTRL_CIRCLE;
		BTN_BACK = SCE_CTRL_CROSS;
	}
}

static float axis(unsigned char raw) {
	float v = ((float)raw - 128.0f) / 127.0f;
	if (v > -0.25f && v < 0.25f)
		return 0.0f;
	return v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
}

void input_update(Input *in) {
	unsigned long long now = sceKernelGetProcessTimeWide();
	SceCtrlData pad;
	memset(&pad, 0, sizeof(pad));
	sceCtrlPeekBufferPositive(0, &pad, 1);
#ifdef IMAGO_AUTOPILOT
	pad.buttons |= autopilot_buttons();
#endif

	in->held = pad.buttons;
	in->pressed = pad.buttons & ~previous;
	in->repeat = in->pressed;
	uint32_t rep = pad.buttons & REPEATABLE;
	if (in->pressed & REPEATABLE) {
		held_since = now;
		last_repeat = now;
	} else if (rep && now - held_since > REPEAT_DELAY_US && now - last_repeat > REPEAT_RATE_US) {
		in->repeat |= rep;
		last_repeat = now;
	}
	previous = pad.buttons;
	in->lx = axis(pad.lx);
	in->ly = axis(pad.ly);

	SceTouchData touch;
	memset(&touch, 0, sizeof(touch));
	sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
	in->tapped = 0;
	in->drag_dy = 0.0f;
	if (touch.reportNum > 0) {
		int x = touch.report[0].x / 2, y = touch.report[0].y / 2; // 1920x1088 -> 960x544
		if (!was_touching) {
			start_x = x;
			start_y = y;
			last_y = y;
			touch_start = now;
			in->dragging = 0;
		}
		if (!in->dragging && (abs(x - start_x) > DRAG_THRESHOLD || abs(y - start_y) > DRAG_THRESHOLD))
			in->dragging = 1;
		if (in->dragging)
			in->drag_dy = (float)(y - last_y);
		last_y = y;
		in->touch_x = x;
		in->touch_y = y;
		in->touching = 1;
		was_touching = 1;
	} else {
		if (was_touching && !in->dragging && now - touch_start < TAP_MAX_US) {
			in->tapped = 1;
			in->tap_x = start_x;
			in->tap_y = start_y;
		}
		in->touching = 0;
		in->dragging = 0;
		was_touching = 0;
	}
}
