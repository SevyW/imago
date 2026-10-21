// Buttons with auto-repeat, analog sticks and front-touch gestures.
#pragma once

#include <psp2/ctrl.h>
#include <stdint.h>

typedef struct {
	uint32_t held;
	uint32_t pressed; // went down this frame
	uint32_t repeat;  // pressed, plus auto-repeat for held d-pad/shoulder buttons
	float lx, ly;     // left stick, -1..1 with a dead zone

	int touching;     // a finger is on the front screen
	int touch_x, touch_y;
	int tapped;       // a short press without movement ended this frame
	int tap_x, tap_y;
	int dragging;     // finger moved far enough to be a drag
	float drag_dy;    // vertical movement this frame while dragging
} Input;

// Confirm/back buttons follow the system setting (O/X swapped in Japan).
extern uint32_t BTN_OK, BTN_BACK;

void input_init(void);
void input_update(Input *in);
#ifdef IMAGO_AUTOPILOT
// The current frame number if autopilot.txt asks for a screenshot of it.
unsigned input_autopilot_shot(void);
#endif
