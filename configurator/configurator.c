/*
 * The Dark Meadow - configurator (companion app in the same LiveArea bubble).
 *
 * Started from the "Settings" button on the game's LiveArea page: the loader (source/util/init.c) sees the "-config"
 * LiveArea parameter and runs app0:/configurator.bin instead of the game. It edits ux0:data/darkmeadow/config.txt:
 * only the settings listed here are touched (a setting left at its default is removed from the file), every other
 * line is kept as it is.
 *
 * Controls: up/down choose, left/right change, Cross toggles / runs an action, Triangle resets the setting under the
 * cursor, Start saves and starts the game, Circle quits (asks to save if something changed).
 *
 * Built with -DHOST_PREVIEW on a PC, it renders one frame to a .ppm instead (layout check).
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "font.h"

#ifndef DATA_PATH
#define DATA_PATH "ux0:data/darkmeadow/"
#endif
#define CONFIG_PATH DATA_PATH "config.txt"
#define PREFETCH_PATH DATA_PATH "texprefetch.bin"

#define SCREEN_W 960
#define SCREEN_H 544

/* ---------------------------------------------------------------------------------------------------------------- */
/* settings                                                                                                          */

enum { T_HEADER, T_BOOL, T_INT, T_FLOAT, T_CHOICE, T_ACTION, T_KEYS };   /* T_KEYS = buttons, assigned by pressing them */
enum { A_RESET_ALL = 1, A_RESET_PREFETCH };

typedef struct {
    int type;
    const char *key;        /* config.txt name (NULL for headers and actions) */
    const char *label;
    const char *help;
    float def, min, max, step;
    const char *const *choices;   /* T_CHOICE: shown names */
    const char *const *values;    /* T_CHOICE: what is written ("" = the default, nothing written) */
    int nchoices;
    int action;
    float val;              /* current (T_CHOICE: index) */
} item_t;

static const char *const msaa_names[] = { "4x", "2x", "Off" };   /* 4x is the default (first = default) */
static const char *const msaa_vals[] = { "4", "2", "0" };
static const char *const fps_names[] = { "60 fps", "30 fps" };
static const char *const fps_vals[] = { "60", "30" };
static const char *const movie_names[] = { "On", "Off" };
static const char *const movie_vals[] = { "Startup", "off" };
/* button remapping (DM_KEY_*): a T_KEYS item's value is a set of buttons (bit i = key_mask[i]), assigned by
 * selecting it, pressing Cross and then the button (or, for an item that allows several, the buttons together) */
static const char *const key_names[] = { "Cross", "Circle", "Square", "Triangle", "D-pad up", "D-pad down", "D-pad left",
                                         "D-pad right", "L", "R", "Start", "Select" };
static const char *const key_vals[] = { "CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "UP", "DOWN", "LEFT", "RIGHT", "L", "R",
                                        "START", "SELECT" };
#define NKEYS 12
enum { K_CROSS = 1, K_CIRCLE = 2, K_SQUARE = 4, K_TRIANGLE = 8, K_UP = 16, K_DOWN = 32, K_LEFT = 64, K_RIGHT = 128, K_L = 256,
       K_R = 512, K_START = 1024, K_SELECT = 2048 };
/* step 1 = several buttons allowed */
#define KEYS(key, label, help, def, multi) { T_KEYS, key, label, help, def, 0, 0, multi }
static const char *const cdram_names[] = { "Off", "16 MB", "20 MB", "24 MB" };
static const char *const cdram_vals[] = { "0", "16", "20", "24" };

static item_t items[] = {
    { T_HEADER, NULL, "Controls" },
    { T_BOOL, "DM_PAD_CONTROLS", "Physical controls", "Sticks and buttons drive the game (exploration, sword, crossbow and menu profiles). Off: touch screen only.", 1 },
    { T_BOOL, "DM_STICK_LOOK", "Right stick camera", "Off: turn the camera with the touch screen while exploring. On: the right stick turns it.", 0 },
    { T_INT, "DM_LOOK_SPEED", "Camera speed", "How fast the right stick turns the camera while exploring (pixels per second at full tilt).", 250, 100, 2000, 10 },
    { T_BOOL, "DM_LOOK_INVERT_X", "Invert camera left/right", "Reverse the right stick's horizontal camera direction.", 0 },
    { T_BOOL, "DM_LOOK_INVERT_Y", "Invert camera up/down", "Reverse the right stick's vertical camera direction.", 0 },
    { T_INT, "DM_CURSOR_SPEED", "Cursor speed", "How fast the left stick moves the cursor (exploration, menus, crossbow aim).", 420, 100, 1500, 10 },
    { T_INT, "DM_SCROLL_SPEED", "Menu scroll speed", "How fast the right stick scrolls lists in menus.", 900, 200, 1500, 50 },

    { T_HEADER, NULL, "Buttons" },
    KEYS("DM_KEY_TAP", "Tap / block", "Tap at the cursor (exploring, menus, crossbow aim: hold and release to shoot); block in sword fights.", K_CROSS, 0),
    KEYS("DM_KEY_TAP_ONLY", "Tap", "Tap at the cursor only (no block). Can be on several buttons, each working alone: press them together when assigning.", K_L | K_R, 1),
    KEYS("DM_KEY_BACK", "Back", "The back button in menus.", K_CIRCLE, 0),
    KEYS("DM_KEY_BLOCK", "Block", "Block in sword fights (as well as Tap / block).", K_DOWN, 0),
    KEYS("DM_KEY_DODGE_LEFT", "Dodge left", "Dodge left in sword and crossbow fights.", K_LEFT, 0),
    KEYS("DM_KEY_DODGE_RIGHT", "Dodge right", "Dodge right in sword and crossbow fights.", K_RIGHT, 0),
    KEYS("DM_KEY_MEDKIT", "Medkit", "Use a medkit in fights.", K_TRIANGLE, 0),
    KEYS("DM_KEY_BOMB", "Bomb", "Throw a bomb in fights.", K_SQUARE, 0),
    KEYS("DM_KEY_MENU", "Menu", "Open the game's menu (back in menus).", K_START, 0),
    KEYS("DM_KEY_TOGGLE", "Controls on/off", "Turn the physical controls off (no cursor, touch screen only) and on again.", K_SELECT, 0),

    { T_HEADER, NULL, "Display" },
    { T_CHOICE, "DM_MSAA", "MSAA", "Multisample anti-aliasing: smoother edges at a cost in frame rate and memory.", 0, 0, 0, 0, msaa_names, msaa_vals, 3 },
    { T_CHOICE, "DM_FPS_LIMIT", "Frame rate limit", "30 fps gives steadier pacing and saves battery.", 0, 0, 0, 0, fps_names, fps_vals, 2 },

    { T_HEADER, NULL, "Performance" },
    { T_BOOL, "DM_SAVE_BEHIND", "Save in the background", "Saves are written to the memory card by a background thread instead of freezing the game (about 70 ms per write). Off: the game waits for each write.", 1 },
    { T_BOOL, "DM_GC_SKIP_REPEAT", "Skip repeated clean-ups", "Skip a garbage collection that comes less than 2 s after the last one (30-70 ms stutters, mostly as fights end). Level changes always clean up.", 1 },
    { T_BOOL, "DM_PIN_THREADS", "Pin threads to cores", "Experimental. Game thread alone on core 0, rendering on core 1, everything else on cores 1-2, instead of letting the system move them.", 0 },

    { T_HEADER, NULL, "Loading" },
    { T_BOOL, "DM_PACKAGE_PREFETCH", "Package prefetch", "Unpack the next areas' and enemies' packages in the background, from what earlier sessions loaded (pkgprefetch.txt).", 1 },

    { T_HEADER, NULL, "Movies" },
    { T_CHOICE, "DM_STARTUP_MOVIE", "Opening movie", "Play the opening movie just before the title screen (needs movies/Startup.mp4).", 0, 0, 0, 0, movie_names, movie_vals, 2 },

    { T_HEADER, NULL, "Diagnostics" },
    { T_BOOL, "DM_UE3_LOG", "Engine log", "Copy the engine's own messages into log.txt.", 1 },
    { T_BOOL, "DM_LOG_TOUCHES", "Log touches", "Write where each finger goes down on the touch screen to log.txt.", 0 },

    { T_HEADER, NULL, "Reset" },
    { T_ACTION, NULL, "Forget recorded textures and packages", "Delete texprefetch.bin and pkgprefetch.txt: both prefetches start recording again from scratch.", 0, 0, 0, 0, NULL, NULL, 0, A_RESET_PREFETCH },
    { T_ACTION, NULL, "All settings to defaults", "Set everything on this page back to its default (other lines in config.txt are kept).", 0, 0, 0, 0, NULL, NULL, 0, A_RESET_ALL },
};
#define NITEMS (int)(sizeof(items) / sizeof(items[0]))

static int dirty;
static char status[160];

static void set_default(item_t *it) { it->val = it->def; }   /* T_CHOICE: def is the index (0 for most) */

static int is_default(const item_t *it) {
    if (it->type == T_CHOICE) return (int)it->val == (int)it->def;
    float d = it->val - it->def;
    return d < 0.001f && d > -0.001f;
}

static void format_value(const item_t *it, char *out, int n) {
    switch (it->type) {
    case T_BOOL: snprintf(out, n, "%s", it->val != 0 ? "On" : "Off"); break;
    case T_INT: {
        const char *k = it->key, *u = "";
        size_t kl = strlen(k);
        if (kl > 3 && !strcmp(k + kl - 3, "_MB")) u = it->val == 0 ? " (off)" : " MB";
        else if (strstr(k, "SPEED")) u = " px/s";
        else if (strstr(k, "LENGTH")) u = " px";
        else if (kl > 3 && !strcmp(k + kl - 3, "_MS")) u = " ms";
        snprintf(out, n, "%d%s", (int)it->val, u);
        break;
    }
    case T_FLOAT: snprintf(out, n, "%.2f", (double)it->val); break;
    case T_CHOICE: snprintf(out, n, "%s", it->choices[(int)it->val]); break;
    case T_KEYS: {
        int m = (int)it->val, o = 0, shared = 0;   /* two functions on one button: both happen (allowed, but worth seeing) */
        for (int i = 0; i < NITEMS; i++)
            if (&items[i] != it && items[i].type == T_KEYS && ((int)items[i].val & m)) shared = 1;
        out[0] = 0;
        for (int k = 0; k < NKEYS && o < n - 12; k++)
            if (m & (1 << k)) o += snprintf(out + o, n - o, "%s%s", o ? " or " : "", key_names[k]);   /* each one works alone */
        if (!m) snprintf(out, n, "None");
        else if (shared && o < n - 10) snprintf(out + o, n - o, " (shared)");
        break;
    }
    default: out[0] = 0;
    }
}

static void write_value(const item_t *it, char *out, int n) {
    switch (it->type) {
    case T_BOOL: snprintf(out, n, "%d", it->val != 0); break;
    case T_INT: snprintf(out, n, "%d", (int)it->val); break;
    case T_FLOAT: snprintf(out, n, "%.2f", (double)it->val); break;
    case T_CHOICE: snprintf(out, n, "%s", it->values[(int)it->val]); break;
    case T_KEYS: {
        int m = (int)it->val, o = 0;
        out[0] = 0;
        for (int k = 0; k < NKEYS && o < n - 10; k++)
            if (m & (1 << k)) o += snprintf(out + o, n - o, "%s%s", o ? "+" : "", key_vals[k]);
        if (!m) snprintf(out, n, "NONE");
        break;
    }
    default: out[0] = 0;
    }
}

/* the whole file, line by line, so lines this app does not manage are written back unchanged */
#define MAX_LINES 512
static char *lines[MAX_LINES];
static int nlines;

static int line_key(const char *line, char *key, int n) {
    while (*line == ' ' || *line == '\t') line++;
    int i = 0;
    while (line[i] && !isspace((unsigned char)line[i]) && i < n - 1) { key[i] = line[i]; i++; }
    key[i] = 0;
    return i;
}

static void load_config(void) {
    for (int i = 0; i < NITEMS; i++) set_default(&items[i]);
    nlines = 0;
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;
    char buf[512];
    while (fgets(buf, sizeof(buf), f) && nlines < MAX_LINES) {
        buf[strcspn(buf, "\r\n")] = 0;
        lines[nlines++] = strdup(buf);
        char key[64], val[64] = "";
        if (!line_key(buf, key, sizeof(key))) continue;
        sscanf(buf, "%*s %63s", val);
        for (int i = 0; i < NITEMS; i++) {
            item_t *it = &items[i];
            if (!it->key || strcmp(it->key, key)) continue;
            if (it->type == T_CHOICE) {
                for (int c = 0; c < it->nchoices; c++) if (!strcmp(it->values[c], val)) it->val = (float)c;
                if (!strcmp(it->key, "DM_STARTUP_MOVIE") && strcmp(val, "off")) it->val = 0;
            } else if (it->type == T_KEYS) {
                int m = 0;
                char tmp[64], *save = NULL;
                snprintf(tmp, sizeof(tmp), "%s", val);
                for (char *t = strtok_r(tmp, "+", &save); t; t = strtok_r(NULL, "+", &save))
                    for (int k = 0; k < NKEYS; k++) if (!strcasecmp(t, key_vals[k])) m |= 1 << k;
                it->val = (float)m;
            } else if (it->type == T_FLOAT) it->val = (float)atof(val);
            else if (it->type == T_BOOL) it->val = atoi(val) != 0;
            else it->val = (float)atoi(val);
        }
    }
    fclose(f);
}

static int save_config(void) {
    int written[NITEMS];
    memset(written, 0, sizeof(written));
    FILE *f = fopen(CONFIG_PATH ".tmp", "w");
    if (!f) return 0;
    for (int l = 0; l < nlines; l++) {
        char key[64];
        int managed = -1;
        if (line_key(lines[l], key, sizeof(key)))
            for (int i = 0; i < NITEMS; i++) if (items[i].key && !strcmp(items[i].key, key)) managed = i;
        if (managed < 0) { fprintf(f, "%s\n", lines[l]); continue; }
        if (written[managed] || is_default(&items[managed])) continue;   /* defaults are not written */
        char v[64];
        write_value(&items[managed], v, sizeof(v));
        fprintf(f, "%s %s\n", items[managed].key, v);
        written[managed] = 1;
    }
    for (int i = 0; i < NITEMS; i++) {
        if (!items[i].key || written[i] || is_default(&items[i])) continue;
        char v[64];
        write_value(&items[i], v, sizeof(v));
        fprintf(f, "%s %s\n", items[i].key, v);
    }
    int ok = fclose(f) == 0;
    if (ok) {
        remove(CONFIG_PATH);
        ok = rename(CONFIG_PATH ".tmp", CONFIG_PATH) == 0;
    }
    if (ok) {   /* the file now holds what was written: reload it as the new base */
        for (int l = 0; l < nlines; l++) free(lines[l]);
        load_config();
        dirty = 0;
    }
    return ok;
}

static void change(item_t *it, int dir) {
    switch (it->type) {
    case T_BOOL: it->val = it->val != 0 ? 0 : 1; break;
    case T_INT:
    case T_FLOAT: {
        float v = it->val + dir * it->step;
        if (v < it->min) v = it->min;
        if (v > it->max) v = it->max;
        it->val = v;
        break;
    }
    case T_CHOICE: it->val = (float)(((int)it->val + dir + it->nchoices) % it->nchoices); break;
    default: return;
    }
    dirty = 1;
}

static void run_action(item_t *it) {
    if (it->action == A_RESET_ALL) {
        for (int i = 0; i < NITEMS; i++) set_default(&items[i]);
        dirty = 1;
        snprintf(status, sizeof(status), "All settings set to defaults (not saved yet).");
    } else if (it->action == A_RESET_PREFETCH) {
        int r = remove(PREFETCH_PATH), r2 = remove(DATA_PATH "pkgprefetch.txt");
        snprintf(status, sizeof(status), (r == 0 || r2 == 0) ? "Recorded textures and packages deleted." : "Nothing recorded to delete.");
    }
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* drawing: rectangles and text, through a small batch of textured quads                                            */

#define MAXQ 8192
static float qv[MAXQ * 6 * 8];   /* x, y, u, v, r, g, b, a */
static int nq;

static void quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t c) {
    if (nq >= MAXQ) return;
    float r = (c >> 24 & 255) / 255.f, g = (c >> 16 & 255) / 255.f, b = (c >> 8 & 255) / 255.f, a = (c & 255) / 255.f;
    float p[6][4] = { { x, y, u0, v0 }, { x + w, y, u1, v0 }, { x, y + h, u0, v1 },
                      { x + w, y, u1, v0 }, { x + w, y + h, u1, v1 }, { x, y + h, u0, v1 } };
    float *o = qv + nq * 48;
    for (int k = 0; k < 6; k++) {
        o[k * 8 + 0] = p[k][0]; o[k * 8 + 1] = p[k][1]; o[k * 8 + 2] = p[k][2]; o[k * 8 + 3] = p[k][3];
        o[k * 8 + 4] = r; o[k * 8 + 5] = g; o[k * 8 + 6] = b; o[k * 8 + 7] = a;
    }
    nq++;
}

#define TEX_W 256
#define TEX_H 256
static void glyph_uv(int ch, float *u0, float *v0, float *u1, float *v1) {
    if (ch < 32 || ch > 127) ch = '?';
    int i = ch - 32, x = (i % FONT_COLS) * FONT_CW, y = (i / FONT_COLS) * FONT_CH;
    *u0 = (float)x / TEX_W; *v0 = (float)y / TEX_H;
    *u1 = (float)(x + FONT_CW) / TEX_W; *v1 = (float)(y + FONT_CH) / TEX_H;
}

static void rect(float x, float y, float w, float h, uint32_t c) {
    float u0, v0, u1, v1;
    glyph_uv(127, &u0, &v0, &u1, &v1);   /* the solid block: sample its middle */
    float cu = (u0 + u1) * 0.5f, cv = (v0 + v1) * 0.5f;
    quad(x, y, w, h, cu, cv, cu, cv, c);
}

static int text(float x, float y, const char *s, uint32_t c) {
    int n = 0;
    for (; *s; s++, n++) {
        float u0, v0, u1, v1;
        glyph_uv((unsigned char)*s, &u0, &v0, &u1, &v1);
        if (*s != ' ') quad(x + n * FONT_CW, y, FONT_CW, FONT_CH, u0, v0, u1, v1, c);
    }
    return n * FONT_CW;
}

/* word-wrapped text, at most `maxl` lines */
static void text_wrap(float x, float y, int cols, int maxl, const char *s, uint32_t c) {
    char line[128];
    for (int l = 0; l < maxl && *s; l++) {
        int len = (int)strlen(s), take = len <= cols ? len : cols;
        if (take < len) { int b = take; while (b > 0 && s[b] != ' ') b--; if (b > 0) take = b; }
        snprintf(line, sizeof(line), "%.*s", take, s);
        text(x, y + l * FONT_CH, line, c);
        s += take;
        while (*s == ' ') s++;
    }
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* the page                                                                                                          */

#define C_BG      0x101014ffu
#define C_PANEL   0x1c1d24ffu
#define C_HEADER  0xc9a96eff   /* parchment gold */
#define C_TEXT    0xd8d8d8ffu
#define C_DIM     0x8a8a92ffu
#define C_SEL     0x3a2f22ffu
#define C_SELBAR  0xc9a96effu
#define C_CHANGED 0xe8c47cffu
#define C_TITLE   0xf0f0f0ffu

#define LIST_X 40
#define LIST_Y 70
#define ROW_H 24
#define ROWS 15
#define VALUE_X 620

static int sel = 1, top = 0;
static int confirm_quit;

static int capture, cap_frames;   /* waiting for the button(s) of a T_KEYS item */
static uint32_t cap_mask, cap_seen;

static void draw_page(void) {
    nq = 0;
    rect(0, 0, SCREEN_W, SCREEN_H, C_BG);
    text(LIST_X, 18, "The Dark Meadow - Settings", C_TITLE);
    const char *saved = dirty ? "unsaved changes" : "saved";
    text(SCREEN_W - 40 - (int)strlen(saved) * FONT_CW, 18, saved, dirty ? C_CHANGED : C_DIM);
    rect(LIST_X, 52, SCREEN_W - 2 * LIST_X, 2, C_HEADER);

    if (sel < top) top = sel;
    if (sel >= top + ROWS) top = sel - ROWS + 1;
    if (top > 0 && items[top - 1].type == T_HEADER && sel - (top - 1) < ROWS) top--;   /* keep a section's header */
    for (int r = 0; r < ROWS && top + r < NITEMS; r++) {
        item_t *it = &items[top + r];
        float y = LIST_Y + r * ROW_H;
        if (it->type == T_HEADER) {
            text(LIST_X, y + 1, it->label, C_HEADER);
            continue;
        }
        if (top + r == sel) {
            rect(LIST_X - 8, y, SCREEN_W - 2 * LIST_X + 16, ROW_H, C_SEL);
            rect(LIST_X - 8, y, 4, ROW_H, C_SELBAR);
        }
        text(LIST_X + 22, y + 1, it->label, C_TEXT);
        char v[48];
        if (it->type == T_ACTION) snprintf(v, sizeof(v), "[ Cross ]");
        else if (it->type == T_KEYS && capture && top + r == sel)
            snprintf(v, sizeof(v), "%s", it->step ? "[ press the button(s) ]" : "[ press a button ]");
        else if (it->type == T_KEYS) format_value(it, v, sizeof(v));
        else {
            char b[40];
            format_value(it, b, sizeof(b));
            if (it->type == T_BOOL || top + r != sel) snprintf(v, sizeof(v), "%s", b);
            else snprintf(v, sizeof(v), "<  %s  >", b);
        }
        text(VALUE_X, y + 1, v, is_default(it) || it->type == T_ACTION ? C_TEXT : C_CHANGED);
    }
    if (top > 0) text(SCREEN_W - LIST_X - FONT_CW, LIST_Y, "^", C_DIM);
    if (top + ROWS < NITEMS) text(SCREEN_W - LIST_X - FONT_CW, LIST_Y + (ROWS - 1) * ROW_H, "v", C_DIM);

    /* help for the selected setting */
    float hy = LIST_Y + ROWS * ROW_H + 8;
    rect(LIST_X - 8, hy, SCREEN_W - 2 * LIST_X + 16, 2 * FONT_CH + 12, C_PANEL);
    const item_t *it = &items[sel];
    if (it->help) text_wrap(LIST_X, hy + 6, (SCREEN_W - 2 * LIST_X) / FONT_CW, 2, it->help, C_DIM);

    float by = SCREEN_H - FONT_CH - 10;
    if (confirm_quit) text(LIST_X, by, "Save changes?   Cross: save and quit   Circle: quit without saving   Square: back", C_CHANGED);
    else if (capture) text(LIST_X, by, it->step ? "Press the button (any, Square and Circle too), or several together; 5 s to cancel" : "Press the button for this (any, Square and Circle too); 5 s to cancel", C_CHANGED);
    else if (status[0]) text(LIST_X, by, status, C_CHANGED);
    else if (it->type == T_KEYS) text(LIST_X, by, "Cross: assign (then press any button)  Square: none  Triangle: default", C_DIM);
    else text(LIST_X, by, "Left/Right: change  Triangle: default  Start: save and play  Circle: quit", C_DIM);
}

static void move_sel(int d) {
    int s = sel;
    do { s += d; } while (s >= 0 && s < NITEMS && items[s].type == T_HEADER);
    if (s >= 0 && s < NITEMS) sel = s;
}

/* ---------------------------------------------------------------------------------------------------------------- */
#ifdef HOST_PREVIEW
/* a software rasteriser for the batch: draws the page into a .ppm (layout check on a PC) */
static uint8_t fb[SCREEN_H][SCREEN_W][3];
static uint8_t atlas[TEX_H][TEX_W];
int main(int argc, char **argv) {
    for (int y = 0; y < FONT_H; y++) for (int x = 0; x < FONT_W; x++) atlas[y][x] = font_alpha[y * FONT_W + x];
    load_config();
    if (argc > 2) sel = atoi(argv[2]);
    if (argc > 3) {   /* save test: camera speed back to default, invert X on, text size 0.7 */
        items[2].val = 900; items[3].val = 1;   /* (host test) */
        printf("save: %d\n", save_config());
        return 0;
    }
    items[3].val = 1; dirty = 1;
    draw_page();
    for (int q = 0; q < nq; q++) {
        float *v = qv + q * 48;
        float x0 = v[0], y0 = v[1], x1 = v[8], y1 = v[17], u0 = v[2], v0 = v[3], u1 = v[10], v1 = v[19];
        for (int y = (int)y0; y < (int)y1; y++) for (int x = (int)x0; x < (int)x1; x++) {
            if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) continue;
            float tu = u0 + (u1 - u0) * ((x - x0 + 0.5f) / (x1 - x0)), tv = v0 + (v1 - v0) * ((y - y0 + 0.5f) / (y1 - y0));
            float a = atlas[(int)(tv * TEX_H)][(int)(tu * TEX_W)] / 255.f * v[7];
            for (int k = 0; k < 3; k++) fb[y][x][k] = (uint8_t)(fb[y][x][k] * (1 - a) + v[4 + k] * 255 * a);
        }
    }
    FILE *f = fopen(argc > 1 ? argv[1] : "preview.ppm", "wb");
    fprintf(f, "P6 %d %d 255\n", SCREEN_W, SCREEN_H);
    fwrite(fb, 1, sizeof(fb), f);
    fclose(f);
    return 0;
}
#else
/* ---------------------------------------------------------------------------------------------------------------- */
/* Vita                                                                                                              */
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

static const uint32_t key_mask[NKEYS] = { SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, SCE_CTRL_SQUARE, SCE_CTRL_TRIANGLE, SCE_CTRL_UP,
                                          SCE_CTRL_DOWN, SCE_CTRL_LEFT, SCE_CTRL_RIGHT, SCE_CTRL_LTRIGGER, SCE_CTRL_RTRIGGER,
                                          SCE_CTRL_START, SCE_CTRL_SELECT };

int _newlib_heap_size_user = 16 * 1024 * 1024;

/* vitaGL (master) calls this vitaShaRK function from vglSetShaderAssociationPath(), which is never used here; older
 * libvitashark.a builds lack it (same weak stub as in the game, source/utils/glutil.c) */
__attribute__((weak)) void shark_set_shader_association_path(const char *path) { (void)path; }

static const char *vsh =
    "attribute vec2 aPos;\n attribute vec2 aUV;\n attribute vec4 aCol;\n"
    "varying vec2 vUV;\n varying vec4 vCol;\n"
    "void main() { vUV = aUV; vCol = aCol; gl_Position = vec4(aPos.x / 480.0 - 1.0, 1.0 - aPos.y / 272.0, 0.0, 1.0); }\n";
static const char *fsh =
    "precision mediump float;\n varying vec2 vUV;\n varying vec4 vCol;\n uniform sampler2D uTex;\n"
    "void main() { gl_FragColor = vec4(vCol.rgb, vCol.a * texture2D(uTex, vUV).a); }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    GLint len = (GLint)strlen(src);
    glShaderSource(s, 1, &src, &len);
    glCompileShader(s);
    return s;
}

int main(void) {
    vglSetShaderCachePath(DATA_PATH "shader_cache");   /* the game's cache: the two small shaders are compiled once */
    vglInitExtended(0, SCREEN_W, SCREEN_H, 4 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, vsh));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fsh));
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aUV");
    glBindAttribLocation(prog, 2, "aCol");
    glLinkProgram(prog);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uTex"), 0);

    static uint8_t px[TEX_W * TEX_H * 4];
    for (int y = 0; y < FONT_H; y++)
        for (int x = 0; x < FONT_W; x++) {
            uint8_t *p = px + (y * TEX_W + x) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = font_alpha[y * FONT_W + x];
        }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX_W, TEX_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    load_config();
    uint32_t old = 0;
    int repeat = 0;
    for (;;) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t b = pad.buttons;
        if (pad.ly < 40) b |= SCE_CTRL_UP;
        if (pad.ly > 216) b |= SCE_CTRL_DOWN;
        if (pad.lx < 40) b |= SCE_CTRL_LEFT;
        if (pad.lx > 216) b |= SCE_CTRL_RIGHT;
        uint32_t pressed = b & ~old;
        /* held directions repeat after 0.4 s, every 70 ms */
        uint32_t dirs = SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT;
        if (b & dirs & old) { if (++repeat > 24 && repeat % 4 == 0) pressed |= b & dirs; }
        else repeat = 0;
        old = b;

        item_t *it = &items[sel];
        if (pressed) status[0] = 0;
        if (capture) {   /* assigning buttons */
            uint32_t raw = pad.buttons;
            int m = 0;
            for (int k = 0; k < NKEYS; k++) if (raw & key_mask[k]) m |= 1 << k;
            if (!cap_seen) { if (!m) cap_seen = 1; }   /* first let go of the Cross that started it */
            else if (m) {
                if (!it->step) { int k = 0; while (!(m & (1 << k))) k++; it->val = (float)(1 << k); dirty = 1; capture = 0; }
                else cap_mask |= (uint32_t)m;
            } else if (cap_mask) { it->val = (float)cap_mask; dirty = 1; capture = 0; }
            if (capture && ++cap_frames > 300) { capture = 0; snprintf(status, sizeof(status), "Not changed."); }
            if (!capture) old = b;   /* the buttons used here do nothing else */
        } else if (confirm_quit) {
            if (pressed & SCE_CTRL_CROSS) { save_config(); break; }
            if (pressed & SCE_CTRL_CIRCLE) break;
            if (pressed & SCE_CTRL_SQUARE) confirm_quit = 0;
        } else {
            if (pressed & SCE_CTRL_UP) move_sel(-1);
            if (pressed & SCE_CTRL_DOWN) move_sel(1);
            if (pressed & SCE_CTRL_LEFT) change(it, -1);
            if (pressed & SCE_CTRL_RIGHT) change(it, 1);
            if (pressed & SCE_CTRL_CROSS) {
                if (it->type == T_ACTION) run_action(it);
                else if (it->type == T_BOOL) change(it, 1);
                else if (it->type == T_KEYS) { capture = 1; cap_frames = 0; cap_mask = 0; cap_seen = 0; }
            }
            if ((pressed & SCE_CTRL_SQUARE) && it->type == T_KEYS && (int)it->val) { it->val = 0; dirty = 1; }
            if ((pressed & SCE_CTRL_TRIANGLE) && it->key && !is_default(it)) { set_default(it); dirty = 1; }
            if (pressed & SCE_CTRL_START) {
                if (!dirty || save_config()) sceAppMgrLoadExec("app0:/eboot.bin", NULL, NULL);
                else snprintf(status, sizeof(status), "Could not write %s", CONFIG_PATH);
            }
            if (pressed & SCE_CTRL_CIRCLE) { if (dirty) confirm_quit = 1; else break; }
        }

        draw_page();
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 32, &qv[0]);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 32, &qv[2]);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 32, &qv[4]);
        glDrawArrays(GL_TRIANGLES, 0, nq * 6);
        vglSwapBuffers(GL_FALSE);
    }
    sceKernelExitProcess(0);
    return 0;
}
#endif
