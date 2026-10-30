/*
 * Physical controls: the sticks and buttons drive the game through made-up touches, the way a finger would. The touch
 * screen keeps working at the same time; these fingers have their own pointer ids.
 *
 * The controls follow what the game is doing (hooks/game_state.c):
 *   menu            a game screen other than the in-game ones is up (pause menu, store, journal, save files, dialogs,
 *                   fight summary, title screens)
 *   sword fight     the player is in a Melee... or Block... state
 *   crossbow fight  ... in a Ranged... state
 *   cutscene        no game screen drawn for 2 s (the HUD is hidden), a cutscene track playing, or the camera swinging
 *                   back after a fight
 *   exploration     otherwise
 *
 *   exploration     left stick: moves the cursor.  Tap (Cross): touch at the cursor (walk / use; held = drag).
 *                   Right stick (only with "Right stick camera" on): look around - a finger dragged from the middle
 *                   of the screen, faster the further the stick is pushed (see "Looking around" below).
 *   sword fight     right stick: a flick = one sword swipe through the middle of the screen, in the stick's direction;
 *                   back to the middle before the next one.  Dodge: D-pad left / right.  Block: Cross or D-pad down
 *                   (held).  Medkit: Triangle.  Bomb: Square.
 *   crossbow fight  left stick: moves the cursor.  Cross: hold at the cursor, steer with the left stick, release to
 *                   shoot.  Dodge, medkit and bomb as in sword fights.
 *   menu            left stick: moves the cursor (drawn by overlay.c).  Cross: touch at the cursor (held = drag).
 *                   Right stick: scroll (a drag from the cursor).  Circle / Start: back.
 *   everywhere      Start: the menu icon at the top of the screen.  L or R: tap at the cursor (never a block).
 *                   Select: physical controls off / on.
 *
 * Every function can be moved to another button with DM_KEY_<function> <button>[+<button>...] in config.txt (the
 * settings app's press-to-assign screen writes these). Other settings: DM_PAD_CONTROLS, DM_STICK_LOOK, DM_LOOK_SPEED,
 * DM_LOOK_INVERT_X / _Y, DM_CURSOR_SPEED, DM_SCROLL_SPEED, DM_LOG_TOUCHES.
 */
#include "input/padinput.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include "android/engine.h"
#include "audio/song.h"
#include "config.h"
#include "hooks/game_state.h"
#include "util/logger.h"
#include "video/movie.h"

#define SCREEN_W 960.f
#define SCREEN_H 544.f
#define CX 480.f
#define CY 272.f

#define DEADZONE 0.18f
#define TAP_GUARD 14.f          /* px a drag must cover before its finger goes down */
#define LIFT_DIST 40.f          /* a look finger is lifted only this far from where it went down ... */
#define LIFT_US 350000ull       /* ... and this long after, so a look is never taken for a tap (a walk) */
#define STILL_US 150000ull      /* ... and after holding still this long, so the camera is not left gliding */
#define EDGE 90.f               /* recentre a drag finger this far from the screen edge */

#define SWIPE_LENGTH 320.f
#define SWIPE_US 80000ull
#define FLICK_ON 0.85f          /* stick pushed this far = a swipe ... */
#define FLICK_OFF 0.35f         /* ... and back under this before the next one */
#define PRESS_MIN_US 60000ull   /* a button touch lasts at least this long, so the game sees it */

#define MENU_HOLD_US 1500000ull       /* a menu drawn this recently = menu controls (menus are not drawn every frame) */
#define MENU_STILL_US 10000000ull     /* a menu that is the last thing drawn stays a menu this long without redraws */
#define MENU_LIVE_US 250000ull        /* a menu not drawn for this long: the right stick no longer scrolls */
#define SCENE_GAP_US 2000000ull       /* no game screen at all for this long = a cutscene (or a loading screen) */
#define POST_FIGHT_US 8000000ull      /* no sticks while the camera swings back after a fight, at most this long */
#define CAMERA_SETTLE_US 200000ull    /* the camera still this long = the game has finished moving it */
#define CAMERA_BLOCK_MAX_US 4000000ull

#define ID_LOOK  0x40000001
#define ID_SWIPE 0x40000002
#define ID_BUTTON 0x40000010    /* + function index */

/* ---- functions and the buttons they are on ---- */
enum { F_TAP, F_TAP_ONLY, F_BACK, F_BOMB, F_MEDKIT, F_BLOCK, F_DODGE_LEFT, F_DODGE_RIGHT, F_MENU, F_TOGGLE, NF };

/* The Vita's L / R are reported as L1 / R1 by sceCtrlPeekBufferPositiveExt2 (L2 / R2 on a PS TV pad) */
#define PAD_L (SCE_CTRL_LTRIGGER | SCE_CTRL_L1)
#define PAD_R (SCE_CTRL_RTRIGGER | SCE_CTRL_R1)

static const struct { const char *name; uint32_t mask; } buttons[] = {
    { "CROSS", SCE_CTRL_CROSS }, { "CIRCLE", SCE_CTRL_CIRCLE }, { "SQUARE", SCE_CTRL_SQUARE },
    { "TRIANGLE", SCE_CTRL_TRIANGLE }, { "UP", SCE_CTRL_UP }, { "DOWN", SCE_CTRL_DOWN }, { "LEFT", SCE_CTRL_LEFT },
    { "RIGHT", SCE_CTRL_RIGHT }, { "L", PAD_L }, { "R", PAD_R }, { "START", SCE_CTRL_START },
    { "SELECT", SCE_CTRL_SELECT },
};
#define NB (int)(sizeof(buttons) / sizeof(buttons[0]))

static struct { const char *key; uint32_t pad; } functions[NF] = {
    [F_TAP] = { "TAP", SCE_CTRL_CROSS },               /* tap at the cursor; block in sword fights */
    [F_TAP_ONLY] = { "TAP_ONLY", PAD_L | PAD_R },      /* tap at the cursor, nothing in sword fights */
    [F_BACK] = { "BACK", SCE_CTRL_CIRCLE },
    [F_BOMB] = { "BOMB", SCE_CTRL_SQUARE },
    [F_MEDKIT] = { "MEDKIT", SCE_CTRL_TRIANGLE },
    [F_BLOCK] = { "BLOCK", SCE_CTRL_DOWN },
    [F_DODGE_LEFT] = { "DODGE_LEFT", SCE_CTRL_LEFT },
    [F_DODGE_RIGHT] = { "DODGE_RIGHT", SCE_CTRL_RIGHT },
    [F_MENU] = { "MENU", SCE_CTRL_START },
    [F_TOGGLE] = { "TOGGLE", SCE_CTRL_SELECT },
};

/* the functions whose buttons are held */
static uint32_t held_functions(uint32_t pad) {
    uint32_t f = 0;
    for (int i = 0; i < NF; i++)
        if (functions[i].pad & pad) f |= 1u << i;
    return f;
}

/* ---- where each function touches, per set of controls ---- */
enum { C_EXPLORE, C_FIGHT, C_MENU, C_CROSSBOW, C_CUTSCENE, NC };   /* overlay.c draws the cursor in 0, 2 and 3 */
#define C_OFF (-1)
static const char *const control_names[NC] = { "exploration", "sword fight", "menu", "crossbow fight", "cutscene" };

#define NO { -1, -1 }
#define AT_CURSOR { -2, -2 }
/* Read off screenshots. Fight screen: block shield (480, 492), dodge arrows (50, 512) / (912, 512), medkit (55, 130),
 * bomb (893, 128), menu icon (480, 28). Menus: the back button at the top left (80, 38). */
static const float places[NC][NF][2] = {
    [C_EXPLORE] = { [F_TAP] = AT_CURSOR, [F_TAP_ONLY] = AT_CURSOR, [F_BACK] = NO, [F_BOMB] = NO, [F_MEDKIT] = NO,
                    [F_BLOCK] = NO, [F_DODGE_LEFT] = NO, [F_DODGE_RIGHT] = NO, [F_MENU] = { 480, 28 }, [F_TOGGLE] = NO },
    [C_FIGHT] = { [F_TAP] = { 480, 492 }, [F_TAP_ONLY] = NO, [F_BACK] = NO, [F_BOMB] = { 893, 128 },
                  [F_MEDKIT] = { 55, 130 }, [F_BLOCK] = { 480, 492 }, [F_DODGE_LEFT] = { 50, 512 },
                  [F_DODGE_RIGHT] = { 912, 512 }, [F_MENU] = { 480, 28 }, [F_TOGGLE] = NO },
    [C_MENU] = { [F_TAP] = AT_CURSOR, [F_TAP_ONLY] = AT_CURSOR, [F_BACK] = { 80, 38 }, [F_BOMB] = NO, [F_MEDKIT] = NO,
                 [F_BLOCK] = NO, [F_DODGE_LEFT] = NO, [F_DODGE_RIGHT] = NO, [F_MENU] = { 80, 38 }, [F_TOGGLE] = NO },
    [C_CROSSBOW] = { [F_TAP] = AT_CURSOR, [F_TAP_ONLY] = AT_CURSOR, [F_BACK] = NO, [F_BOMB] = { 893, 128 },
                     [F_MEDKIT] = { 55, 130 }, [F_BLOCK] = NO, [F_DODGE_LEFT] = { 50, 512 },
                     [F_DODGE_RIGHT] = { 912, 512 }, [F_MENU] = { 480, 28 }, [F_TOGGLE] = NO },
    [C_CUTSCENE] = { [F_TAP] = NO, [F_TAP_ONLY] = NO, [F_BACK] = NO, [F_BOMB] = NO, [F_MEDKIT] = NO, [F_BLOCK] = NO,
                     [F_DODGE_LEFT] = NO, [F_DODGE_RIGHT] = NO, [F_MENU] = { 480, 28 }, [F_TOGGLE] = NO },
};
#define IS_OFF(c, f) (places[c][f][0] == -1.f)
#define IS_CURSOR(c, f) (places[c][f][0] == -2.f)

/* ---- settings ---- */
static int enabled = 1;      /* DM_PAD_CONTROLS */
static int stick_look;       /* DM_STICK_LOOK: the right stick turns the camera while exploring (off: the touch screen) */
static int invert_x, invert_y;
static float look_speed = 250.f, cursor_speed = 420.f, scroll_speed = 900.f;   /* px/s at full tilt */
int padinput_log_touches;

/* shared with overlay.c */
volatile int padinput_profile = C_OFF;
volatile float padinput_cursor_x = CX, padinput_cursor_y = CY;

static void load_settings(void) {
    enabled = config_bool("DM_PAD_CONTROLS", 1);
    stick_look = config_bool("DM_STICK_LOOK", 0);
    look_speed = config_float("DM_LOOK_SPEED", look_speed);
    cursor_speed = config_float("DM_CURSOR_SPEED", cursor_speed);
    scroll_speed = config_float("DM_SCROLL_SPEED", scroll_speed);
    invert_x = config_bool("DM_LOOK_INVERT_X", 0);
    invert_y = config_bool("DM_LOOK_INVERT_Y", 0);
    padinput_log_touches = config_bool("DM_LOG_TOUCHES", 0);

    /* DM_KEY_<function> <button>[+<button>...], or "NONE" */
    for (int f = 0; f < NF; f++) {
        char key[32];
        snprintf(key, sizeof(key), "DM_KEY_%s", functions[f].key);
        const char *v = config_str(key, NULL);
        if (!v) continue;
        uint32_t pad = 0;
        char list[64], *save = NULL;
        snprintf(list, sizeof(list), "%s", v);
        for (char *t = strtok_r(list, "+ ", &save); t; t = strtok_r(NULL, "+ ", &save))
            for (int b = 0; b < NB; b++)
                if (!strcasecmp(t, buttons[b].name)) pad |= buttons[b].mask;
        functions[f].pad = pad;

        char names[64] = "";
        for (int b = 0; b < NB; b++)
            if ((pad & buttons[b].mask) == buttons[b].mask)
                snprintf(names + strlen(names), sizeof(names) - strlen(names), "%s%s", names[0] ? "+" : "", buttons[b].name);
        l_info("[pad] %s on %s", functions[f].key, names[0] ? names : "no button");
    }
}

void padinput_init(void) {
    load_settings();
    if (!enabled) {
        l_info("[pad] physical controls off");
        return;
    }
    l_info("[pad] physical controls: right stick camera %s, look %.0f px/s%s%s, cursor %.0f px/s, scroll %.0f px/s",
           stick_look ? "on" : "off", (double)look_speed, invert_x ? " (x reversed)" : "", invert_y ? " (y reversed)" : "",
           (double)cursor_speed, (double)scroll_speed);
    padinput_profile = C_EXPLORE;
}

/* ---- sticks ---- */
static float stick(uint8_t raw) { return ((float)raw - 128.f) / 128.f; }

/* Dead zone, then 0..1 from its edge to full tilt; (x, y) becomes the direction. */
static float shape(float *x, float *y) {
    float m = sqrtf(*x * *x + *y * *y);
    if (m < DEADZONE) {
        *x = *y = 0;
        return 0;
    }
    float t = (m - DEADZONE) / (1.f - DEADZONE);
    if (t > 1.f) t = 1.f;
    *x /= m;
    *y /= m;
    return t;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- looking around (and scrolling menus): a dragged finger ---- */
static struct {
    int down;             /* the finger is on the screen */
    int closing;          /* the controls changed: finish lifting it */
    uint64_t t_down, t_still;
    float ax, ay;         /* where drags start */
    float sx, sy;         /* where this finger went down */
    float fx, fy;         /* where it is */
    float ox, oy, pending;   /* movement collected before it goes down (TAP_GUARD) */
    float lx, ly;         /* last direction */
} look;
static uint64_t poll_now;

static void finger_down(float x, float y) {
    engine_touch(0, ID_LOOK, x, y);
    look.sx = x;
    look.sy = y;
    look.t_down = poll_now;
}

/* Lift the look finger. Unless forced, only once it is LIFT_DIST from where it went down, LIFT_US old and has held
 * still for STILL_US: a lift that is not ready yet is retried on the next poll. */
static void look_lift(int force) {
    if (look.down) {
        float dx = look.fx - look.sx, dy = look.fy - look.sy, d = sqrtf(dx * dx + dy * dy);
        if (d < LIFT_DIST) {
            float ux = d > 0.5f ? dx / d : look.lx, uy = d > 0.5f ? dy / d : look.ly;
            if (ux == 0.f && uy == 0.f) ux = 1.f;
            look.fx = clampf(look.sx + ux * LIFT_DIST, 1.f, SCREEN_W - 2.f);
            look.fy = clampf(look.sy + uy * LIFT_DIST, 1.f, SCREEN_H - 2.f);
            engine_touch(2, ID_LOOK, look.fx, look.fy);
        }
        if (!force && poll_now - look.t_down < LIFT_US) return;
        if (!look.t_still) look.t_still = poll_now;
        if (!force && poll_now - look.t_still < STILL_US) {
            engine_touch(2, ID_LOOK, look.fx, look.fy);
            return;
        }
        engine_touch(1, ID_LOOK, look.fx, look.fy);
    }
    memset(&look, 0, sizeof(look));
}

/* Drag from (ax, ay). follow = 1: the finger moves the way the stick is pushed (looking around); 0: against it
 * (scrolling, so the list moves the way the stick is pushed). */
static void look_update(float sx, float sy, float dt, float speed, float ax, float ay, int follow) {
    float t = shape(&sx, &sy);
    if (look.closing || t <= 0.f) {
        look_lift(0);
        return;
    }
    look.t_still = 0;
    float v = t * t * speed * dt;
    if (!follow) {
        sx = -sx;
        sy = -sy;
    }
    float dx = (invert_x ? -sx : sx) * v, dy = (invert_y ? -sy : sy) * v;
    float d = sqrtf(dx * dx + dy * dy);
    if (d > 0.f) {
        look.lx = dx / d;
        look.ly = dy / d;
    }
    if (!look.down) {
        look.ax = ax;
        look.ay = ay;
        look.ox += dx;
        look.oy += dy;
        look.pending += d;
        if (look.pending < TAP_GUARD) return;
        look.down = 1;
        finger_down(look.ax, look.ay);
        look.fx = clampf(look.ax + look.ox, 1.f, SCREEN_W - 2.f);
        look.fy = clampf(look.ay + look.oy, 1.f, SCREEN_H - 2.f);
        engine_touch(2, ID_LOOK, look.fx, look.fy);
        return;
    }
    look.fx += dx;
    look.fy += dy;
    if (look.fx < EDGE || look.fx > SCREEN_W - EDGE || look.fy < EDGE || look.fy > SCREEN_H - EDGE) {
        /* near the edge: lift, and put the finger back where the drag started (a long drag: never a tap) */
        engine_touch(1, ID_LOOK, clampf(look.fx, 1.f, SCREEN_W - 2.f), clampf(look.fy, 1.f, SCREEN_H - 2.f));
        look.fx = look.ax;
        look.fy = look.ay;
        finger_down(look.fx, look.fy);
        return;
    }
    engine_touch(2, ID_LOOK, look.fx, look.fy);
}

/* The right stick waits while the game moves the camera itself (after fights, menus, text boxes, while walking to a
 * tapped spot): a stick drag then turns it the wrong way. Paused when the camera turns with no look finger of ours
 * down (and not just after one: the drag's own glide), or when exploration starts again; back once the camera has
 * held still for CAMERA_SETTLE_US (at least `min_us` after the pause, at most CAMERA_BLOCK_MAX_US) and the stick has
 * returned to the middle. */
static struct { int paused; uint64_t since, min_us, last_lift; } camera;

static void camera_pause(uint64_t now, const char *why, uint64_t min_us) {
    if (!camera.paused) {
        static int logged;
        if (logged++ < 300) l_info("[pad] right stick look paused: %s", why);
    }
    camera.paused = 1;
    camera.since = now;
    camera.min_us = min_us;
}

static int camera_free(uint64_t now, float rx, float ry) {
    uint64_t moved = game_state.camera_moved_us;
    int moving = moved && now > moved && now - moved < CAMERA_SETTLE_US;
    if (!look.down && moving && now - camera.last_lift > 400000ull) camera_pause(now, "the camera is moving by itself", 0);
    if (!camera.paused) return 1;
    int settled = (!moving && now - camera.since >= camera.min_us) || now - camera.since > CAMERA_BLOCK_MAX_US;
    if (!settled || shape(&rx, &ry) > 0.f) return 0;
    camera.paused = 0;
    static int logged;
    if (logged++ < 300) l_info("[pad] right stick look back (camera settled after %u ms)", (unsigned)((now - camera.since) / 1000));
    return 1;
}

static void stick_camera(uint64_t now, float rx, float ry, float dt) {
    int was_down = look.down;
    if (camera_free(now, rx, ry)) look_update(rx, ry, dt, look_speed, CX, CY, 1);
    else look_lift(0);
    if (was_down && !look.down) camera.last_lift = now;
}

/* ---- sword swipes ---- */
static struct { int armed, active; uint64_t t0; float sx, sy, ex, ey; } swipe = { .armed = 1 };

static void swipe_release(void) {
    if (swipe.active) engine_touch(1, ID_SWIPE, swipe.ex, swipe.ey);
    swipe.active = 0;
}

static void swipe_update(float rx, float ry, uint64_t now) {
    if (swipe.active) {
        float t = (float)(now - swipe.t0) / (float)SWIPE_US;
        if (t >= 1.f) {
            engine_touch(2, ID_SWIPE, swipe.ex, swipe.ey);
            engine_touch(1, ID_SWIPE, swipe.ex, swipe.ey);
            swipe.active = 0;
        } else {
            engine_touch(2, ID_SWIPE, swipe.sx + (swipe.ex - swipe.sx) * t, swipe.sy + (swipe.ey - swipe.sy) * t);
        }
        return;
    }
    float m = sqrtf(rx * rx + ry * ry);
    if (!swipe.armed) {
        if (m < FLICK_OFF) swipe.armed = 1;
        return;
    }
    if (m < FLICK_ON) return;
    float ux = rx / m, uy = ry / m, h = SWIPE_LENGTH * 0.5f;
    swipe.sx = clampf(CX - ux * h, 1.f, SCREEN_W - 2.f);
    swipe.sy = clampf(CY - uy * h, 1.f, SCREEN_H - 2.f);
    swipe.ex = clampf(CX + ux * h, 1.f, SCREEN_W - 2.f);
    swipe.ey = clampf(CY + uy * h, 1.f, SCREEN_H - 2.f);
    swipe.armed = 0;
    swipe.active = 1;
    swipe.t0 = now;
    engine_touch(0, ID_SWIPE, swipe.sx, swipe.sy);
}

/* ---- cursor ---- */
static void cursor_update(float sx, float sy, float dt) {
    float t = shape(&sx, &sy);
    if (t <= 0.f) return;
    float v = t * t * cursor_speed * dt;
    padinput_cursor_x = clampf(padinput_cursor_x + sx * v, 0.f, SCREEN_W - 1.f);
    padinput_cursor_y = clampf(padinput_cursor_y + sy * v, 0.f, SCREEN_H - 1.f);
}

/* ---- buttons: a touch at the function's place while its button is held ---- */
static struct { int down, released; uint64_t t; float x, y; } touches[NF];

static void buttons_release(void) {
    for (int f = 0; f < NF; f++)
        if (touches[f].down) engine_touch(1, ID_BUTTON + f, touches[f].x, touches[f].y);
    memset(touches, 0, sizeof(touches));
}

static void buttons_update(int c, uint32_t held, uint64_t now) {
    for (int f = 0; f < NF; f++) {
        int on = (held >> f) & 1;
        if (on && !touches[f].down && !IS_OFF(c, f)) {
            int cursor = IS_CURSOR(c, f);
            float x = cursor ? padinput_cursor_x : places[c][f][0], y = cursor ? padinput_cursor_y : places[c][f][1];
            if (look.down && fabsf(x - look.fx) < 60.f && fabsf(y - look.fy) < 60.f) look_lift(1);   /* one finger there */
            engine_touch(0, ID_BUTTON + f, x, y);
            touches[f] = (typeof(touches[f])){ .down = 1, .t = now, .x = x, .y = y };
        } else if (on && touches[f].down && IS_CURSOR(c, f) &&
                   (touches[f].x != padinput_cursor_x || touches[f].y != padinput_cursor_y)) {   /* held: drag */
            touches[f].x = padinput_cursor_x;
            touches[f].y = padinput_cursor_y;
            engine_touch(2, ID_BUTTON + f, touches[f].x, touches[f].y);
        } else if (!on && touches[f].down) {
            touches[f].released = 1;
        }
        if (touches[f].down && touches[f].released && now - touches[f].t >= PRESS_MIN_US) {
            engine_touch(1, ID_BUTTON + f, touches[f].x, touches[f].y);
            touches[f].down = touches[f].released = 0;
        }
    }
}

/* ---- which controls ---- */
static int choose_controls(uint64_t now) {
    uint64_t menu = game_state.menu_drawn_us, hud = game_state.hud_drawn_us;
    /* the fight summary is gone once the player controller leaves its post-fight state, though it is still the last
     * screen drawn */
    int summary_closed = game_state.post_fight_exit_us > menu;

    if (menu && now - menu < MENU_HOLD_US && !summary_closed) return C_MENU;
    /* a menu that nothing redraws (inventory, pause, dialogs) stays a menu while it is the last thing drawn */
    if (menu && menu > hud && now - menu < MENU_STILL_US && !song_cin_playing() && !summary_closed) return C_MENU;
    if (!menu && !hud) return C_CUTSCENE;   /* none of the game's own screens yet (boot, opening movie) */

    /* fight music stands in only if the hook on the player's state did not go in */
    int fight = game_state.fight_seen ? game_state.fight : (song_battle_music() ? FIGHT_SWORD : FIGHT_NONE);
    if (fight == FIGHT_SWORD) return C_FIGHT;
    if (fight == FIGHT_CROSSBOW) return C_CROSSBOW;

    uint64_t end = game_state.fight_end_us;   /* the camera swings back after a fight */
    if (game_state.post_fight && end && now > end && now - end < POST_FIGHT_US) return C_CUTSCENE;

    if (now - (hud > menu ? hud : menu) > SCENE_GAP_US || song_cin_playing()) return C_CUTSCENE;
    return C_EXPLORE;
}

void padinput_poll(void) {
    if (!enabled) return;
    static uint64_t last;
    static int off, select_was;
    uint64_t now = sceKernelGetProcessTimeWide();
    poll_now = now;
    float dt = last ? (float)(now - last) / 1e6f : 0.f;
    if (dt > 0.05f) dt = 0.05f;
    last = now;

    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositiveExt2(0, &pad, 1);
    if (movie_playing()) {   /* the movie player reads the buttons itself (skip) */
        pad.buttons = 0;
        pad.lx = pad.ly = pad.rx = pad.ry = 128;
    }
    uint32_t held = held_functions(pad.buttons);

    int select = (held >> F_TOGGLE) & 1;
    if (select && !select_was) {
        off = !off;
        l_info("[pad] physical controls %s", off ? "off" : "on");
    }
    select_was = select;

    int c = off ? C_OFF : choose_controls(now);
    if (look.closing) look_lift(0);
    if (c != padinput_profile) {
        /* lift every made-up finger before the controls change meaning (the look finger over the next polls) */
        if (look.down) {
            look.closing = 1;
            look_lift(0);
        }
        swipe_release();
        buttons_release();
        static int logged;
        if (logged++ < 200)
            l_info("[pad] %s controls%s%s", c == C_OFF ? "no" : control_names[c], c == C_MENU ? " - screen: " : "",
                   c == C_MENU ? (const char *)game_state.menu_name : "");
        if (c == C_EXPLORE && stick_look) camera_pause(now, "exploration again (after a menu, fight or cutscene)", 400000ull);
        padinput_profile = c;
    }
    if (c == C_OFF) return;

    float lx = stick(pad.lx), ly = stick(pad.ly), rx = stick(pad.rx), ry = stick(pad.ry);
    switch (c) {
    case C_EXPLORE:
        cursor_update(lx, ly, dt);
        if (stick_look) stick_camera(now, rx, ry, dt);
        break;
    case C_FIGHT:
        swipe_update(rx, ry, now);
        break;
    case C_CROSSBOW:
        cursor_update(lx, ly, dt);
        break;
    case C_CUTSCENE:
        break;
    case C_MENU:
        cursor_update(lx, ly, dt);
        if (strstr((const char *)game_state.menu_name, "CombatSummary")) {
            /* the level / gold / kill streak texts after a fight are drawn over the game, which still takes drags as
             * camera look: the right stick is the camera there (a scroll drag turned it the opposite way) */
            if (stick_look) stick_camera(now, rx, ry, dt);
            else look_lift(0);
        } else if (now - game_state.menu_drawn_us < MENU_LIVE_US) {
            look_update(rx, ry, dt, scroll_speed, padinput_cursor_x, padinput_cursor_y, 0);
        } else {
            look_lift(0);   /* the menu is closing: a scroll drag would reach the game as a look */
        }
        break;
    }
    buttons_update(c, held, now);
}
