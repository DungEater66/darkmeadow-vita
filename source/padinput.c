/*
 * Physical controls (r59, r60): the sticks and buttons drive the game through made-up touches, the way a finger would.
 * The touch screen keeps working at the same time; these fingers have their own pointer ids.
 *
 * Five profiles, chosen from the game's state (r60, r61):
 *   menu            a game screen other than the gameplay ones is up (pause menu, store, journal, save files, dialogs,
 *                   combat summary, title screens: see ui_scene in reimpl/ue3_trace.c)
 *   sword fight     the player's combat pawn is in a Melee... or Block... state  (ui_combat_mode, GotoState hook)
 *   crossbow fight  ... in a Ranged... state
 *   cutscene        no game screen drawn for 2 s (the HUD is hidden) or a cutscene track (cin_*) playing (r61)
 *   exploration     otherwise
 *
 *   exploration     left stick        moves the cursor;  Cross: touch at the cursor (walk / use; held = drag)
 *                   right stick       look around: a finger dragged from the middle of the screen, faster the further
 *                                     the stick is pushed. It goes down only once the stick has moved it a little,
 *                                     and is lifted only 40 px from where it went down and 350 ms after, so a look
 *                                     never counts as a tap. Put back in the middle before the screen edge.
 *   sword fight     right stick       a flick = one sword swipe through the middle of the screen, in the stick's
 *                                     direction (any angle); back to the middle before the next one
 *                   D-pad left/right  dodge;  Cross or D-pad down: block (held);  Triangle: medkit;  Square: bomb
 *   crossbow fight  left stick        moves the cursor;  Cross: touch at the cursor - hold, steer with the left
 *                                     stick, release to shoot;  D-pad left/right: dodge;  Triangle: medkit;
 *                                     Square: bomb;  right stick: nothing
 *   menu            left stick        moves a cursor (drawn by overlay.c);  Cross: touch at the cursor (held =
 *                                     drag);  right stick: scroll (a drag from the cursor);  Circle / Start: back
 *   exploration / fights: Start = the menu icon at the top of the screen.  L / R: nothing (config.txt can set them).
 *
 * config.txt: DM_PAD_CONTROLS 0 (off), DM_LOOK_SPEED, DM_LOOK_INVERT_X/Y, DM_CURSOR_SPEED, DM_SCROLL_SPEED,
 * DM_SWIPE_LENGTH, DM_SWIPE_MS, DM_SWIPE_CENTER x y, DM_<EXPLORE|FIGHT|CROSSBOW|MENU|BUTTON>_<button> x y | cursor | off
 * (BUTTON = all profiles; buttons: CROSS CIRCLE SQUARE TRIANGLE UP DOWN LEFT RIGHT L R START),
 * DM_LOG_TOUCHES 1 (log real touches, to read positions off).
 */
#include "padinput.h"
#include "engine.h"
#include "movie.h"
#include "song.h"
#include "utils/logger.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#ifndef DATA_PATH
#define DATA_PATH "ux0:data/darkmeadow/"
#endif

#define SCREEN_W 960.f
#define SCREEN_H 544.f
#define CX 480.f
#define CY 272.f
#define DEADZONE 0.18f
#define TAP_GUARD 14.f          /* px a drag must cover before its finger goes down */
#define LIFT_DIST 40.f          /* r60: a look finger is lifted only this far from where it went down ... */
#define LIFT_US 350000ull       /* ... and this long after (r59's short looks were taken for taps: walking) */
#define EDGE 90.f               /* recentre a drag finger this far from the screen edge */
#define FLICK_ON 0.85f
#define FLICK_OFF 0.35f
#define PRESS_MIN_US 60000ull
#define MENU_HOLD_US 1500000ull /* a menu scene drawn this recently = menu profile (r61: 250 ms flickered: menus are
                                 * not drawn every frame) */
#define SCENE_GAP_US 2000000ull /* r61: no game screen at all for this long = a cutscene (or a loading screen) */

#define ID_LOOK  0x40000001
#define ID_SWIPE 0x40000002
#define ID_BTN   0x40000010     /* + button index */

extern volatile uint64_t ui_hud_us, ui_menu_us;   /* reimpl/ue3_trace.c */
extern volatile char ui_menu_name[64];
extern volatile int ui_combat_mode, ui_combat_seen;
/* 0 none, 1 sword, 2 crossbow (the player's combat state) */
extern volatile uint64_t ui_fight_end_us;   /* r64: a fight ended (ue3_trace.c) */
extern volatile int ui_post_combat;         /* r64: the player controller is in PostCombatTransition */
extern volatile uint64_t ui_post_exit_us;   /* r65: when it left it (ue3_trace.c) */
extern volatile uint64_t ui_cam_moved_us;   /* r71: the camera turned in the last frame drawn (ue3_trace.c) */
#define CAM_SETTLE_US 200000ull   /* r71: the camera still this long = the game has finished moving it */
#define CAM_BLOCK_MAX_US 4000000ull
#define MENU_LIVE_US 250000ull    /* r65: a menu not drawn for this long: the right stick no longer scrolls */
#define POST_FIGHT_US 8000000ull  /* r64: no sticks while the camera swings back after a fight, at most this long */
#define STILL_US 150000ull        /* r64: a look finger is held still this long before it lifts, so no fling is left */

static int stick_look;   /* r69: DM_STICK_LOOK 1 - the right stick turns the camera while exploring (default: the touch
                         * screen does; the stick look fought the game's own camera moves after fights and cutscenes) */
static int enabled = 1, paused;   /* r64: paused = Select turned the controls off */
#define P_OFF (-1)
int padinput_log_touches;
static float look_speed = 250.f, cursor_speed = 420.f, scroll_speed = 900.f;   /* r62 defaults (were 900/650/700) */
static float swipe_len = 320.f, swipe_cx = CX, swipe_cy = CY;
static int invert_x, invert_y, swipe_ms = 80;   /* r62: 90 -> 80 */

enum { B_CROSS, B_CIRCLE, B_SQUARE, B_TRIANGLE, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_L, B_R, B_START, NB };
static const char *const bname[NB] = { "CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "UP", "DOWN", "LEFT", "RIGHT",
                                       "L", "R", "START" };
static const uint32_t bmask[NB] = { SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, SCE_CTRL_SQUARE, SCE_CTRL_TRIANGLE, SCE_CTRL_UP,
                                    SCE_CTRL_DOWN, SCE_CTRL_LEFT, SCE_CTRL_RIGHT, SCE_CTRL_LTRIGGER, SCE_CTRL_RTRIGGER,
                                    SCE_CTRL_START };

enum { P_EXPLORE, P_FIGHT, P_MENU, P_CROSSBOW, P_CUTSCENE, NP };   /* overlay.c: cursor in 0, 2, 3 */
static const char *const pname[NP] = { "exploration", "sword fight", "menu", "crossbow fight", "cutscene" };
static const char *const pkey[NP] = { "EXPLORE", "FIGHT", "MENU", "CROSSBOW", "CUTSCENE" };
#define OFF -1.f
#define CUR -2.f   /* at the menu cursor */
/* Where each button touches, per profile, read off screenshots: fight screen - block shield (480, 492) for Cross and
 * D-pad down, dodge arrows (50, 512) / (912, 512) for D-pad left / right, medkit (55, 130) for Triangle, bomb (893, 128)
 * for Square, menu icon (480, 28) for Start; menus - the back button at the top left (80, 38) for Circle and Start. */
/* r65: the L column is the "tap only" function (tap at the cursor; nothing in sword fights), which is on L and R by
 * default (DM_KEY_TAP_ONLY L+R, see the remapping below); the R column is unused */
static float bx[NP][NB] = {
    /*             CROSS CIRCLE SQUARE TRIANGLE UP   DOWN LEFT RIGHT L    R    START */
    [P_EXPLORE]  = { CUR, OFF,   OFF,   OFF,     OFF, OFF, OFF, OFF,  CUR, OFF, 480 },
    [P_FIGHT]    = { 480, OFF,   893,   55,      OFF, 480, 50,  912,  OFF, OFF, 480 },
    [P_MENU]     = { CUR, 80,    OFF,   OFF,     OFF, OFF, OFF, OFF,  CUR, OFF, 80  },
    [P_CROSSBOW] = { CUR, OFF,   893,   55,      OFF, OFF, 50,  912,  CUR, OFF, 480 },
    [P_CUTSCENE] = { OFF, OFF,   OFF,   OFF,     OFF, OFF, OFF, OFF,  OFF, OFF, 480 },
};
static float by[NP][NB] = {
    [P_EXPLORE]  = { CUR, OFF,   OFF,   OFF,     OFF, OFF, OFF, OFF,  CUR, OFF, 28 },
    [P_FIGHT]    = { 492, OFF,   128,   130,     OFF, 492, 512, 512,  OFF, OFF, 28 },
    [P_MENU]     = { CUR, 38,    OFF,   OFF,     OFF, OFF, OFF, OFF,  CUR, OFF, 38 },
    [P_CROSSBOW] = { CUR, OFF,   128,   130,     OFF, OFF, 512, 512,  CUR, OFF, 28 },
    [P_CUTSCENE] = { OFF, OFF,   OFF,   OFF,     OFF, OFF, OFF, OFF,  OFF, OFF, 28 },
};

/* r65: remapping. Each function of the default layout (named after the button it is on) can be put on any physical
 * button, or none, with DM_KEY_<function> <button> in config.txt (the settings app writes these). The physical
 * buttons are turned into the default layout's buttons before anything else looks at them. */
static const char *const phys_name[] = { "CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "UP", "DOWN", "LEFT", "RIGHT", "L", "R",
                                          "START", "SELECT" };
/* r69: sceCtrlPeekBufferPositiveExt2 reports the Vita's L / R as L1 / R1 (LTRIGGER / RTRIGGER are L2 / R2 there):
 * r65-r68 looked only at L2 / R2, so L and R did nothing */
#define PHYS_L (SCE_CTRL_LTRIGGER | SCE_CTRL_L1)
#define PHYS_R (SCE_CTRL_RTRIGGER | SCE_CTRL_R1)
static const uint32_t phys_mask[] = { SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, SCE_CTRL_SQUARE, SCE_CTRL_TRIANGLE, SCE_CTRL_UP,
                                      SCE_CTRL_DOWN, SCE_CTRL_LEFT, SCE_CTRL_RIGHT, PHYS_L, PHYS_R,
                                      SCE_CTRL_START, SCE_CTRL_SELECT };
static struct { const char *key; uint32_t virt; uint32_t phys; } fn[] = {
    { "TAP", SCE_CTRL_CROSS, SCE_CTRL_CROSS },               /* tap at the cursor; block in sword fights */
    { "TAP_ONLY", SCE_CTRL_LTRIGGER, PHYS_L | PHYS_R },     /* tap at the cursor only; L and R */
    { "BACK", SCE_CTRL_CIRCLE, SCE_CTRL_CIRCLE },            /* back in menus */
    { "BOMB", SCE_CTRL_SQUARE, SCE_CTRL_SQUARE },
    { "MEDKIT", SCE_CTRL_TRIANGLE, SCE_CTRL_TRIANGLE },
    { "BLOCK", SCE_CTRL_DOWN, SCE_CTRL_DOWN },
    { "DODGE_LEFT", SCE_CTRL_LEFT, SCE_CTRL_LEFT },
    { "DODGE_RIGHT", SCE_CTRL_RIGHT, SCE_CTRL_RIGHT },
    { "MENU", SCE_CTRL_START, SCE_CTRL_START },
    { "TOGGLE", SCE_CTRL_SELECT, SCE_CTRL_SELECT },           /* physical controls off / on */
};
#define NFN (int)(sizeof(fn) / sizeof(fn[0]))

static uint32_t remap(uint32_t phys) {
    uint32_t v = 0;
    for (int f = 0; f < NFN; f++) if (fn[f].phys & phys) v |= fn[f].virt;
    return v;
}

/* shared with overlay.c */
volatile int padinput_profile = -1;   /* -1 = off */
volatile float padinput_cursor_x = CX, padinput_cursor_y = CY;

/* ---- config ---- */
static void set_button(int p, int b, const char *a, const char *c, int n) {
    if (!strcmp(a, "off")) bx[p][b] = by[p][b] = OFF;
    else if (!strcmp(a, "cursor")) bx[p][b] = by[p][b] = CUR;
    else if (n == 3) { bx[p][b] = (float)atof(a); by[p][b] = (float)atof(c); }
}

static void load_config(void) {
    FILE *f = fopen(DATA_PATH "config.txt", "r");
    if (!f) return;
    char line[160], k[64], a[32], c[32];
    while (fgets(line, sizeof(line), f)) {
        int n = sscanf(line, "%63s %31s %31s", k, a, c);
        if (n < 2) continue;
        if (!strcmp(k, "DM_PAD_CONTROLS")) enabled = atoi(a) != 0;
        else if (!strcmp(k, "DM_LOOK_SPEED")) look_speed = (float)atof(a);
        else if (!strcmp(k, "DM_STICK_LOOK")) stick_look = atoi(a) != 0;
        else if (!strcmp(k, "DM_CURSOR_SPEED")) cursor_speed = (float)atof(a);
        else if (!strcmp(k, "DM_SCROLL_SPEED")) scroll_speed = (float)atof(a);
        else if (!strcmp(k, "DM_LOOK_INVERT_X")) invert_x = atoi(a) != 0;
        else if (!strcmp(k, "DM_LOOK_INVERT_Y")) invert_y = atoi(a) != 0;
        else if (!strcmp(k, "DM_SWIPE_LENGTH")) swipe_len = (float)atof(a);
        else if (!strcmp(k, "DM_SWIPE_MS")) swipe_ms = atoi(a) > 10 ? atoi(a) : 10;
        else if (!strcmp(k, "DM_SWIPE_CENTER") && n == 3) { swipe_cx = (float)atof(a); swipe_cy = (float)atof(c); }
        else if (!strcmp(k, "DM_LOG_TOUCHES")) padinput_log_touches = atoi(a) != 0;
        else if (!strncmp(k, "DM_KEY_", 7)) {   /* r65 */
            for (int f = 0; f < NFN; f++) {
                if (strcmp(k + 7, fn[f].key)) continue;
                fn[f].phys = 0;   /* one button, or several joined with + (CROSS+L+R) */
                char tmp[32], *save = NULL;
                snprintf(tmp, sizeof(tmp), "%s", a);
                for (char *t = strtok_r(tmp, "+", &save); t; t = strtok_r(NULL, "+", &save))
                    for (int b = 0; b < (int)(sizeof(phys_mask) / sizeof(phys_mask[0])); b++)
                        if (!strcasecmp(t, phys_name[b])) fn[f].phys |= phys_mask[b];
            }
        }
        else if (!strncmp(k, "DM_", 3)) {
            for (int p = -1; p < NP; p++) {
                const char *pre = p < 0 ? "BUTTON" : pkey[p];
                size_t pl = strlen(pre);
                if (strncmp(k + 3, pre, pl) || k[3 + pl] != '_') continue;
                for (int b = 0; b < NB; b++) {
                    if (strcmp(k + 4 + pl, bname[b])) continue;
                    if (p < 0) for (int q = 0; q < NP; q++) set_button(q, b, a, c, n);
                    else set_button(p, b, a, c, n);
                }
            }
        }
    }
    fclose(f);
}

static void describe(int p, char *m, int sz) {
    int o = 0;
    for (int b = 0; b < NB && o < sz - 32; b++) {
        if (bx[p][b] == OFF) continue;
        if (bx[p][b] == CUR) o += snprintf(m + o, sz - o, "%s%s at the cursor", o ? ", " : "", bname[b]);
        else o += snprintf(m + o, sz - o, "%s%s (%d, %d)", o ? ", " : "", bname[b], (int)bx[p][b], (int)by[p][b]);
    }
    if (!o) snprintf(m, sz, "none");
}

void padinput_init(void) {
    load_config();
    if (!enabled) { l_info("[pad] physical controls off (DM_PAD_CONTROLS 0)"); return; }
    l_info("[pad] physical controls: look %.0f px/s%s%s, cursor %.0f px/s, swipe %.0f px in %d ms through (%.0f, %.0f)",
           (double)look_speed, invert_x ? " (x reversed)" : "", invert_y ? " (y reversed)" : "", (double)cursor_speed,
           (double)swipe_len, swipe_ms, (double)swipe_cx, (double)swipe_cy);
    for (int p = 0; p < NP; p++) {
        char m[400];
        describe(p, m, sizeof(m));
        l_info("[pad]   %s buttons: %s", pname[p], m);
    }
    for (int f = 0; f < NFN; f++) {   /* r65: remapped functions */
        if (fn[f].phys == fn[f].virt || (fn[f].virt == SCE_CTRL_LTRIGGER && fn[f].phys == (PHYS_L | PHYS_R)))
            continue;
        char to[64] = "";
        for (int b = 0; b < (int)(sizeof(phys_mask) / sizeof(phys_mask[0])); b++)
            if ((fn[f].phys & phys_mask[b]) == phys_mask[b]) snprintf(to + strlen(to), sizeof(to) - strlen(to), "%s%s", to[0] ? "+" : "", phys_name[b]);
        l_info("[pad]   remapped: %s on %s (DM_KEY_%s)", fn[f].key, to[0] ? to : "nothing", fn[f].key);
    }
    padinput_profile = P_EXPLORE;
}

static float stick(uint8_t raw) { return ((float)raw - 128.f) / 128.f; }

/* dead zone, then a curve: fine control near the middle, full speed at the edge. Returns 0..1, (x, y) normalised. */
static float shape(float *x, float *y) {
    float m = sqrtf(*x * *x + *y * *y);
    if (m < DEADZONE) { *x = *y = 0; return 0; }
    float t = (m - DEADZONE) / (1.f - DEADZONE);
    if (t > 1.f) t = 1.f;
    *x /= m;
    *y /= m;
    return t;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- drag (look around; scroll in menus) ---- */
static struct { int down, lifting, closing; uint64_t t_down, t_still; float ax, ay, sx, sy, fx, fy, ox, oy, pend, moved, lx, ly; } look;
static uint64_t pad_now;

static void finger_down(float x, float y) {
    engine_touch(0, ID_LOOK, x, y);
    look.sx = x;
    look.sy = y;
    look.t_down = pad_now;
}

/* lift now (force) or once the finger is LIFT_DIST from where it went down and LIFT_US old: never a tap */
static void look_release_ex(int force) {
    if (look.down) {
        float dx = look.fx - look.sx, dy = look.fy - look.sy, d = sqrtf(dx * dx + dy * dy);
        if (d < LIFT_DIST) {
            float ux = d > 0.5f ? dx / d : look.lx, uy = d > 0.5f ? dy / d : look.ly;
            if (ux == 0.f && uy == 0.f) ux = 1.f;
            look.fx = clampf(look.sx + ux * LIFT_DIST, 1.f, SCREEN_W - 2.f);
            look.fy = clampf(look.sy + uy * LIFT_DIST, 1.f, SCREEN_H - 2.f);
            engine_touch(2, ID_LOOK, look.fx, look.fy);
        }
        if (!force && pad_now - look.t_down < LIFT_US) { look.lifting = 1; return; }   /* hold still a little longer */
        /* r64: and hold still before lifting at all - lifting a finger that was moving left the camera with the
         * drag's speed (a full turn on the way into the lift) */
        if (!look.t_still) look.t_still = pad_now;
        if (!force && pad_now - look.t_still < STILL_US) { engine_touch(2, ID_LOOK, look.fx, look.fy); look.lifting = 1; return; }
        engine_touch(1, ID_LOOK, look.fx, look.fy);
    }
    memset(&look, 0, sizeof(look));
}
static void look_release(void) { look_release_ex(1); }

/* r71: the right stick look waits while the game moves the camera itself. Blocked when the camera turns with no
 * look finger of ours down (and not just after one: the drag's own glide), or when exploration starts again (after a
 * menu, a fight, a cutscene); unblocked once the camera has held still for CAM_SETTLE_US (at most CAM_BLOCK_MAX_US) and
 * the stick has come back to the middle. */
static int cam_block;
static uint64_t cam_block_t, look_lift_t, cam_block_min;
static void cam_block_start(uint64_t now, const char *why, uint64_t min_us) {
    cam_block_min = min_us;   /* r72: at least this long (r71: back "after 0 ms" when exploration resumed, and the camera
                               * started its swing a few frames later) */
    if (!cam_block) {
        static int n;
        if (n++ < 300) l_info("[pad] right stick look paused: %s", why);
    }
    cam_block = 1;
    cam_block_t = now;
}
static int cam_look_allowed(uint64_t now, float rx, float ry) {
    uint64_t m = ui_cam_moved_us;
    int moving = m && now > m && now - m < CAM_SETTLE_US;
    if (!look.down && moving && now - look_lift_t > 400000ull) cam_block_start(now, "the camera is moving by itself", 0);
    if (!cam_block) return 1;
    int settled = (!moving && now - cam_block_t >= cam_block_min) || now - cam_block_t > CAM_BLOCK_MAX_US;
    float sx = rx, sy = ry;
    int centred = shape(&sx, &sy) <= 0.f;
    if (settled && centred) {
        cam_block = 0;
        static int n;
        if (n++ < 300) l_info("[pad] right stick look back (camera settled after %u ms)", (unsigned)((now - cam_block_t) / 1000));
        return 1;
    }
    return 0;
}

/* drag from (ax, ay). follow = 1: the finger moves the way the stick is pushed (exploration since r61: the camera
 * turns as it does for a finger dragged that way); 0: against it (menu scrolling) */
static void look_update(float sx, float sy, float dt, float speed, float ax, float ay, int follow) {
    float t = shape(&sx, &sy);
    if (look.closing) { look_release_ex(0); return; }   /* r64: the controls changed: finish lifting first */
    if (t <= 0.f) { look_release_ex(0); return; }
    look.lifting = 0;   /* pushed again while waiting to lift: carry on with the same finger */
    look.t_still = 0;
    float v = t * t * speed * dt;
    if (!follow) { sx = -sx; sy = -sy; }
    float dx = (invert_x ? -sx : sx) * v, dy = (invert_y ? -sy : sy) * v;
    float d = sqrtf(dx * dx + dy * dy);
    if (d > 0.f) { look.lx = dx / d; look.ly = dy / d; }
    if (!look.down) {
        look.ax = ax;
        look.ay = ay;
        look.ox += dx;
        look.oy += dy;
        look.pend += d;
        if (look.pend < TAP_GUARD) return;
        look.down = 1;
        finger_down(look.ax, look.ay);
        look.fx = clampf(look.ax + look.ox, 1.f, SCREEN_W - 2.f);
        look.fy = clampf(look.ay + look.oy, 1.f, SCREEN_H - 2.f);
        look.moved = look.pend;
        engine_touch(2, ID_LOOK, look.fx, look.fy);
        return;
    }
    look.fx += dx;
    look.fy += dy;
    look.moved += d;
    if (look.fx < EDGE || look.fx > SCREEN_W - EDGE || look.fy < EDGE || look.fy > SCREEN_H - EDGE) {
        /* lift, and put the finger back where the drag started (after a long drag: no tap either way) */
        look.fx = clampf(look.fx, 1.f, SCREEN_W - 2.f);
        look.fy = clampf(look.fy, 1.f, SCREEN_H - 2.f);
        engine_touch(1, ID_LOOK, look.fx, look.fy);
        look.fx = look.ax;
        look.fy = look.ay;
        look.moved = 0.f;
        finger_down(look.fx, look.fy);
        return;
    }
    engine_touch(2, ID_LOOK, look.fx, look.fy);
}

/* ---- sword swipe ---- */
static struct { int armed, active; uint64_t t0; float sx, sy, ex, ey; } sw = { 1, 0, 0, 0, 0, 0, 0 };

static void swipe_release(void) {
    if (sw.active) engine_touch(1, ID_SWIPE, sw.ex, sw.ey);
    sw.active = 0;
}

static void swipe_update(float rx, float ry, uint64_t now) {
    float m = sqrtf(rx * rx + ry * ry);
    if (sw.active) {
        float t = (float)(now - sw.t0) / (swipe_ms * 1000.f);
        if (t >= 1.f) {
            engine_touch(2, ID_SWIPE, sw.ex, sw.ey);
            engine_touch(1, ID_SWIPE, sw.ex, sw.ey);
            sw.active = 0;
        } else {
            engine_touch(2, ID_SWIPE, sw.sx + (sw.ex - sw.sx) * t, sw.sy + (sw.ey - sw.sy) * t);
        }
        return;
    }
    if (!sw.armed) { if (m < FLICK_OFF) sw.armed = 1; return; }
    if (m < FLICK_ON) return;
    float ux = rx / m, uy = ry / m, h = swipe_len * 0.5f;
    sw.sx = clampf(swipe_cx - ux * h, 1.f, SCREEN_W - 2.f);
    sw.sy = clampf(swipe_cy - uy * h, 1.f, SCREEN_H - 2.f);
    sw.ex = clampf(swipe_cx + ux * h, 1.f, SCREEN_W - 2.f);
    sw.ey = clampf(swipe_cy + uy * h, 1.f, SCREEN_H - 2.f);
    sw.armed = 0;
    sw.active = 1;
    sw.t0 = now;
    engine_touch(0, ID_SWIPE, sw.sx, sw.sy);
    static int n;
    if (n++ < 10) l_debug("[pad] swipe (%d, %d) -> (%d, %d)", (int)sw.sx, (int)sw.sy, (int)sw.ex, (int)sw.ey);
}

/* ---- menu cursor ---- */
static void cursor_update(float sx, float sy, float dt) {
    float t = shape(&sx, &sy);
    if (t <= 0.f) return;
    float v = t * t * cursor_speed * dt;
    padinput_cursor_x = clampf(padinput_cursor_x + sx * v, 0.f, SCREEN_W - 1.f);
    padinput_cursor_y = clampf(padinput_cursor_y + sy * v, 0.f, SCREEN_H - 1.f);
}

/* ---- buttons ---- */
static struct { int down, want_up; uint64_t t; float x, y; } bs[NB];

static void buttons_release(void) {
    for (int b = 0; b < NB; b++)
        if (bs[b].down) { engine_touch(1, ID_BTN + b, bs[b].x, bs[b].y); bs[b].down = 0; bs[b].want_up = 0; }
}

static void buttons_update(int p, uint32_t held, uint64_t now) {
    for (int b = 0; b < NB; b++) {
        int on = (held & bmask[b]) != 0;
        if (on && !bs[b].down && bx[p][b] != OFF && !(bs[b].want_up)) {
            int cur = bx[p][b] == CUR;
            float x = cur ? padinput_cursor_x : bx[p][b], y = cur ? padinput_cursor_y : by[p][b];
            if (look.down && fabsf(x - look.fx) < 60.f && fabsf(y - look.fy) < 60.f) look_release();   /* one finger there */
            engine_touch(0, ID_BTN + b, x, y);
            bs[b].down = 1;
            bs[b].want_up = 0;
            bs[b].t = now;
            bs[b].x = x;
            bs[b].y = y;
        } else if (on && bs[b].down && bx[p][b] == CUR &&
                   (bs[b].x != padinput_cursor_x || bs[b].y != padinput_cursor_y)) {   /* held at the cursor: drag */
            bs[b].x = padinput_cursor_x;
            bs[b].y = padinput_cursor_y;
            engine_touch(2, ID_BTN + b, bs[b].x, bs[b].y);
        } else if (!on && bs[b].down) {
            bs[b].want_up = 1;
        }
        if (bs[b].down && bs[b].want_up && now - bs[b].t >= PRESS_MIN_US) {
            engine_touch(1, ID_BTN + b, bs[b].x, bs[b].y);
            bs[b].down = 0;
            bs[b].want_up = 0;
        }
    }
}

/* ---- profiles ---- */
/* r60: from the game's state: a menu screen up -> menu; else the player's combat state (sword / crossbow / none);
 * fight music only if the combat state has never been seen (the hook did not go in) */
static int auto_profile(uint64_t now) {
    uint64_t m = ui_menu_us, h = ui_hud_us;
    /* r65: the combat summary is gone once the player controller leaves PostCombatTransition: no menu hold after it
     * (r64: the hold kept the menu profile 1.5 s more, and the right stick scrolled - the camera went the wrong way) */
    if (m && now - m < MENU_HOLD_US && !(ui_post_exit_us > m)) return P_MENU;
    /* r69: a menu that stops being redrawn while nothing changes on it (inventory, pause, dialogs: r68 dropped to the
     * cutscene profile for a moment every few seconds, hiding the cursor and letting go of held buttons) stays a menu
     * while it is the last thing drawn - for up to 10 s, and not once a cutscene track plays */
    if (m && m > h && now - m < 10000000ull && !song_cin_playing() && !(ui_post_exit_us > m)) return P_MENU;
    if (!m && !h) return P_CUTSCENE;   /* r63: nothing of the game's own screens yet (boot, opening movie): no cursor
                                        * until the title screen */
    int fight = ui_combat_seen ? ui_combat_mode : (song_battle_music() ? 1 : 0);
    if (fight == 1) return P_FIGHT;
    if (fight == 2) return P_CROSSBOW;
    /* r64: a fight just over - the player controller is in PostCombatTransition while the game swings the camera
     * back (a few seconds; the right stick's look turned it the wrong way there). The summary screen above is a menu
     * and keeps the cursor. r62 keyed this to the fight screen, which is drawn on after the fight: no controls at all. */
    uint64_t e = ui_fight_end_us;
    if (ui_post_combat && e && now > e && now - e < POST_FIGHT_US) return P_CUTSCENE;
    /* r61: a cutscene - no game screen drawn at all (the HUD is hidden), or a cutscene track playing */
    if ((now - (h > m ? h : m) > SCENE_GAP_US) || song_cin_playing()) return P_CUTSCENE;
    return P_EXPLORE;
}

void padinput_poll(void) {
    if (!enabled) return;
    static uint64_t last;
    uint64_t now = sceKernelGetProcessTimeWide();
    pad_now = now;
    float dt = last ? (float)(now - last) / 1e6f : 0.f;
    if (dt > 0.05f) dt = 0.05f;
    last = now;
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositiveExt2(0, &pad, 1);
    pad.buttons = remap(pad.buttons);   /* r65: to the default layout's buttons */
    if (movie_playing()) {   /* the movie player reads the buttons itself (skip) */
        pad.buttons = 0;
        pad.lx = pad.ly = pad.rx = pad.ry = 128;
    }
    {   /* r64: Select turns the physical controls off (no cursor, the buttons and sticks do nothing; the touch screen
         * works as always) and on again */
        static int sel_was;
        int sel = (pad.buttons & SCE_CTRL_SELECT) != 0;
        if (sel && !sel_was) {
            paused = !paused;
            l_info("[pad] Select: physical controls %s", paused ? "off" : "on");
        }
        sel_was = sel;
    }
    int p = paused ? P_OFF : auto_profile(now);
    if (look.closing) look_release_ex(0);   /* r64: a look finger still being lifted (held still first) */
    if (p != padinput_profile) {
        /* lift every made-up finger before the controls change meaning (r64: the look finger is held still and
         * lifted over the next polls) */
        if (look.down) { look.closing = 1; look_release_ex(0); }
        swipe_release();
        buttons_release();
        /* r61: the cursor is never moved by a profile change (r60 centred it for every menu, e.g. the summary after a
         * fight) */
        static int n;
        if (n++ < 200) l_info("[pad] %s controls%s%s", p == P_OFF ? "no" : pname[p], p == P_MENU ? " - screen: " : "",
                              p == P_MENU ? (const char *)ui_menu_name : "");
        if (p == P_EXPLORE && stick_look) cam_block_start(now, "exploration again (after a menu, fight or cutscene)", 400000ull);   /* r71 */
        padinput_profile = p;
    }

    {   /* r61: how regularly the game's screens are drawn (to tune MENU_HOLD_US / SCENE_GAP_US): longest gap per 10 s */
        static uint64_t win_t, max_gap;
        uint64_t last_scene = ui_hud_us > ui_menu_us ? ui_hud_us : ui_menu_us;
        if (last_scene && now > last_scene && now - last_scene > max_gap) max_gap = now - last_scene;   /* r64: the
                                                                     * screen time can be a little ahead of `now` */
        if (now - win_t > 10000000ull && p != P_OFF) {
            static int n;
            if (win_t && max_gap > 300000ull && n++ < 100)
                l_info("[pad] longest gap between game screens in the last 10 s: %u ms (profile %s)", (unsigned)(max_gap / 1000), pname[p]);
            win_t = now;
            max_gap = 0;
        }
    }
    if (p == P_OFF) return;
    float lx = stick(pad.lx), ly = stick(pad.ly), rx = stick(pad.rx), ry = stick(pad.ry);
    switch (p) {
    case P_EXPLORE:   /* r60: left stick moves the cursor (Cross taps there), right stick looks */
        cursor_update(lx, ly, dt);
        if (stick_look) {
            int was_down = look.down;
            if (cam_look_allowed(now, rx, ry)) look_update(rx, ry, dt, look_speed, CX, CY, 1);
            else if (look.down) look_release_ex(0);   /* held still, then lifted */
            if (was_down && !look.down) look_lift_t = now;
        }
        break;
    case P_FIGHT:
        swipe_update(rx, ry, now);
        break;
    case P_CROSSBOW:   /* r60: aim with the cursor; hold Cross, steer, release to shoot */
        cursor_update(lx, ly, dt);
        break;
    case P_CUTSCENE:   /* r61: no cursor, no sticks; Start still reaches the menu icon */
        break;
    case P_MENU:
        cursor_update(lx, ly, dt);
        /* r65: scroll only while a menu is really being drawn - in the hold after it closes, a scroll drag reached the
         * game as a look drag the opposite way */
        if (strstr((const char *)ui_menu_name, "CombatSummary")) {
            /* r72: the level / gold / exp / kill streak texts after a fight are drawn over the game, which still takes
             * drags as camera look: the right stick is the camera there (or nothing, with the stick camera off). r71
             * scrolled with it - a drag the opposite way, so the camera turned inverted on both axes until the last
             * text faded. */
            if (stick_look && cam_look_allowed(now, rx, ry)) look_update(rx, ry, dt, look_speed, CX, CY, 1);
            else look_update(0.f, 0.f, dt, look_speed, CX, CY, 1);
        } else if (now - ui_menu_us < MENU_LIVE_US) look_update(rx, ry, dt, scroll_speed, padinput_cursor_x, padinput_cursor_y, 0);
        else look_update(0.f, 0.f, dt, scroll_speed, padinput_cursor_x, padinput_cursor_y, 0);
        break;
    }
    buttons_update(p, pad.buttons, now);
}
