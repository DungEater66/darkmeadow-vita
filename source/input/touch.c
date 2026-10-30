/*
 * The front touch screen, as Android touch events: a finger that appears is a "down", one that is still there a
 * "move", one that is gone an "up". Based on the so-loader template's controls.c by Volodymyr Atamanenko (MIT).
 */
#include "input/touch.h"

#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include "android/engine.h"
#include "input/padinput.h"
#include "util/logger.h"

/* the touch panel reports 1920x1088 */
#define TO_SCREEN_X(x) ((float)(x) * 960.f / 1920.f)
#define TO_SCREEN_Y(y) ((float)(y) * 544.f / 1088.f)

enum { ACTION_DOWN = 0, ACTION_UP = 1, ACTION_MOVE = 2 };   /* Android MotionEvent actions */

static SceTouchData now, before;

void touch_init(void) {
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, 1);
}

static int has_finger(const SceTouchData *d, int id) {
    for (int i = 0; i < (int)d->reportNum; i++)
        if (d->report[i].id == id) return 1;
    return 0;
}

void touch_poll(void) {
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &now, 1);
    for (int i = 0; i < (int)now.reportNum; i++) {
        float x = TO_SCREEN_X(now.report[i].x), y = TO_SCREEN_Y(now.report[i].y);
        int down = !has_finger(&before, now.report[i].id);
        if (down && padinput_log_touches) l_info("[touch] finger down at (%d, %d)", (int)x, (int)y);
        engine_touch(down ? ACTION_DOWN : ACTION_MOVE, now.report[i].id, x, y);
    }
    for (int i = 0; i < (int)before.reportNum; i++)
        if (!has_finger(&now, before.report[i].id))
            engine_touch(ACTION_UP, before.report[i].id, TO_SCREEN_X(before.report[i].x), TO_SCREEN_Y(before.report[i].y));
    memcpy(&before, &now, sizeof(now));
}
