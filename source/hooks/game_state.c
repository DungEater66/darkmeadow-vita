/*
 * What the game is doing, for the physical controls (input/padinput.c) and the movie player.
 *
 * Screens: the game's menus and HUD are UHorrorMenuScene objects, drawn every frame by RenderScene_Horror (the base
 * class and four overrides). Each scene drawn is classified by its class name: in-game screens (HUD, pause button,
 * popup and witch text, notifications, subtitles) or a menu (anything else: pause menu, store, journal, pickups,
 * fight summary). Each new scene object is logged once as [ui].
 *
 * Fights and the player: UObject::GotoState is the one function every script state change goes through.
 *   - HorrorPlayerCombatPawn: Melee... / Block... = sword fight, Ranged... = crossbow fight, anything else = none.
 *     The weapon being drawn (HorrorWeapon_Sword / _Crossbow: WeaponEquipping, Active) is the earliest sign of a fight.
 *   - HorrorPC (the player controller): PostCombatTransition or Dead ends a fight, even if the pawn keeps its state.
 *     Every change of its state is logged.
 *   Each (class, state) pair is also logged once, to find new signals.
 *
 * Camera: ULocalPlayer::CalcSceneView computes the camera's final rotation every frame; a frame where it turned more
 * than a hair is noted, so the right stick can wait while the game moves the camera itself.
 */
#include "hooks/game_state.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "hooks/hook.h"
#include "hooks/hooks_internal.h"
#include "util/logger.h"

extern so_module so_mod;

volatile game_state_t game_state;

static uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

/* ---- engine helpers to name objects ---- */
static void (*e_GetFullName)(ue3_fstring_t *out, const void *self, const void *stop);
static void (*e_FNameToString)(ue3_fstring_t *out, const void *fname);
static void (*e_appFree)(void *);

static void fstring_to_utf8(ue3_fstring_t *t, char *out, size_t n) {
    if (t->data && t->num > 0) ue3_wstr_to_utf8(t->data, out, n);
    else snprintf(out, n, "?");
    if (t->data && e_appFree) e_appFree(t->data);
    t->data = NULL;
}

/* "Class Outer.Name" */
static void full_name(const void *obj, char *out, size_t n) {
    snprintf(out, n, "?");
    if (!e_GetFullName) return;
    ue3_fstring_t t = { 0 };
    e_GetFullName(&t, obj, NULL);
    fstring_to_utf8(&t, out, n);
}

/* ---- screens ---- */
static const char *const ingame_screens[] = { "HUD", "PauseButton", "PopupText", "WitchText", "Notification", "Subtitle" };

static int is_ingame_screen(const char *cls) {
    for (size_t i = 0; i < sizeof(ingame_screens) / sizeof(ingame_screens[0]); i++)
        if (strstr(cls, ingame_screens[i])) return 1;
    return 0;
}

#define KNOWN_SCENES 128
static struct { const void *obj; int ingame; char cls[48]; } known[KNOWN_SCENES];
static int known_n, known_ring;
static pthread_mutex_t known_lock = PTHREAD_MUTEX_INITIALIZER;

static void scene_drawn(const void *scene) {
    int ingame = -1;
    char cls[48] = "";
    pthread_mutex_lock(&known_lock);
    for (int i = 0; i < known_n; i++)
        if (known[i].obj == scene) {
            ingame = known[i].ingame;
            snprintf(cls, sizeof(cls), "%s", known[i].cls);
            break;
        }
    pthread_mutex_unlock(&known_lock);

    if (ingame < 0) {
        char full[160];
        full_name(scene, full, sizeof(full));
        char *sp = strchr(full, ' ');
        snprintf(cls, sizeof(cls), "%.*s", sp ? (int)(sp - full) : (int)strlen(full), full);
        ingame = is_ingame_screen(cls);
        pthread_mutex_lock(&known_lock);
        int slot = known_n < KNOWN_SCENES ? known_n++ : (known_ring++ % KNOWN_SCENES);   /* full: replace the oldest */
        known[slot].obj = scene;
        known[slot].ingame = ingame;
        snprintf(known[slot].cls, sizeof(known[slot].cls), "%s", cls);
        pthread_mutex_unlock(&known_lock);
        static int logged;
        if (logged++ < 150) l_info("[ui] scene %s (%s)", full, ingame ? "gameplay" : "menu");
    }

    if (ingame) {
        game_state.hud_drawn_us = now_us();
    } else {
        game_state.menu_drawn_us = now_us();
        for (int i = 0; i < 63; i++) {
            game_state.menu_name[i] = cls[i];
            if (!cls[i]) break;
        }
    }
}

typedef void (*render_fn)(void *, void *, float);
static render_fn o_render[5];
static void h_render0(void *s, void *c, float d) { scene_drawn(s); o_render[0](s, c, d); }
static void h_render1(void *s, void *c, float d) { scene_drawn(s); o_render[1](s, c, d); }
static void h_render2(void *s, void *c, float d) { scene_drawn(s); o_render[2](s, c, d); }
static void h_render3(void *s, void *c, float d) { scene_drawn(s); o_render[3](s, c, d); }
static void h_render4(void *s, void *c, float d) { scene_drawn(s); o_render[4](s, c, d); }

/* ---- state changes (GotoState runs on the game thread only, so these caches need no lock) ---- */
static struct { const void *obj; char cls[40]; } class_cache[64];
static int class_n, class_ring;
static struct { uint32_t idx, num; char name[40]; } name_cache[256];
static int name_n, name_ring;

static const char *class_of(const void *obj) {
    for (int i = 0; i < class_n; i++)
        if (class_cache[i].obj == obj) return class_cache[i].cls;
    char full[96];
    full_name(obj, full, sizeof(full));
    char *sp = strchr(full, ' ');
    if (sp) *sp = 0;
    int k = class_n < 64 ? class_n++ : (class_ring++ % 64);
    class_cache[k].obj = obj;
    snprintf(class_cache[k].cls, sizeof(class_cache[k].cls), "%.39s", full);
    return class_cache[k].cls;
}

static const char *state_name(uint32_t idx, uint32_t num) {
    for (int i = 0; i < name_n; i++)
        if (name_cache[i].idx == idx && name_cache[i].num == num) return name_cache[i].name;
    char st[48] = "?";
    if (e_FNameToString) {
        uint32_t fname[2] = { idx, num };
        ue3_fstring_t t = { 0 };
        e_FNameToString(&t, fname);
        fstring_to_utf8(&t, st, sizeof(st));
    }
    int k = name_n < 256 ? name_n++ : (name_ring++ % 256);
    name_cache[k].idx = idx;
    name_cache[k].num = num;
    snprintf(name_cache[k].name, sizeof(name_cache[k].name), "%.39s", st);
    return name_cache[k].name;
}

static void set_fight(int fight, const char *why) {
    if (fight != game_state.fight)
        l_info("[ui] fight: %s (%s)", fight == FIGHT_SWORD ? "sword" : fight == FIGHT_CROSSBOW ? "crossbow" : "none", why);
    game_state.fight = fight;
    game_state.fight_seen = 1;
}

static void player_controller_state(const char *st) {
    static char last[48];
    if (strcmp(last, st)) {
        static int logged;
        if (logged++ < 400) l_info("[ui] player controller: %s -> %s", last[0] ? last : "-", st);
        snprintf(last, sizeof(last), "%s", st);
    }

    int post = !strcmp(st, "PostCombatTransition");
    if (!post && game_state.post_fight) game_state.post_fight_exit_us = now_us();
    game_state.post_fight = post;
    if (post) game_state.fight_end_us = now_us();

    if ((post || !strcmp(st, "Dead")) && game_state.fight) {
        char why[64];
        snprintf(why, sizeof(why), "player controller %s", st);
        set_fight(FIGHT_NONE, why);
        game_state.fight_end_us = now_us();
    }
}

static void log_pair_once(const char *cls, const char *st) {
    static uint32_t seen[512];
    static int n;
    if (n >= 512) return;
    uint32_t h = 5381;
    for (const char *p = cls; *p; p++) h = h * 33 + (unsigned char)*p;
    for (const char *p = st; *p; p++) h = h * 33 + (unsigned char)*p;
    for (int i = 0; i < n; i++)
        if (seen[i] == h) return;
    seen[n++] = h;
    l_info("[ui] %s goes to state %s", cls, st);
}

static int (*o_GotoState)(void *, uint32_t, uint32_t, uint32_t, uint32_t);
static int h_GotoState(void *self, uint32_t name_index, uint32_t name_number, uint32_t label, uint32_t force) {
    if (self && e_GetFullName && e_FNameToString) {
        const char *cls = class_of(self), *st = state_name(name_index, name_number);
        if (!strcmp(cls, "HorrorPlayerCombatPawn")) {
            int fight = (!strncmp(st, "Melee", 5) || !strncmp(st, "Block", 5)) ? FIGHT_SWORD
                      : !strncmp(st, "Ranged", 6) ? FIGHT_CROSSBOW : FIGHT_NONE;
            char why[64];
            snprintf(why, sizeof(why), "player %s", st);
            set_fight(fight, why);
        } else if ((!strcmp(cls, "HorrorWeapon_Sword") || !strcmp(cls, "HorrorWeapon_Crossbow")) &&
                   (!strcmp(st, "WeaponEquipping") || !strcmp(st, "Active")) && !game_state.fight) {
            char why[64];
            snprintf(why, sizeof(why), "%s %s", cls, st);
            set_fight(cls[13] == 'S' ? FIGHT_SWORD : FIGHT_CROSSBOW, why);
        } else if (!strcmp(cls, "HorrorPC")) {
            player_controller_state(st);
        }
        log_pair_once(cls, st);
    }
    return o_GotoState(self, name_index, name_number, label, force);
}

/* ---- camera (rotation units: 65536 = a full turn) ---- */
#define CAMERA_STILL 24

static void *(*o_CalcSceneView)(void *, void *, float *, int32_t *, void *, void *);
static void *h_CalcSceneView(void *self, void *family, float *location, int32_t *rotation, void *viewport, void *drawer) {
    static int32_t last[2];
    static int have_last;
    void *r = o_CalcSceneView(self, family, location, rotation, viewport, drawer);
    if (rotation) {
        int dp = (int16_t)(uint16_t)(rotation[0] - last[0]), dy = (int16_t)(uint16_t)(rotation[1] - last[1]);
        if (have_last && (dp > CAMERA_STILL || dp < -CAMERA_STILL || dy > CAMERA_STILL || dy < -CAMERA_STILL))
            game_state.camera_moved_us = now_us();
        last[0] = rotation[0];
        last[1] = rotation[1];
        have_last = 1;
    }
    return r;
}

void game_state_install(void) {
    e_GetFullName = (void *)so_symbol(&so_mod, "_ZNK7UObject11GetFullNameEPKS_");
    e_FNameToString = (void *)so_symbol(&so_mod, "_ZNK5FName8ToStringEv");
    e_appFree = (void *)so_symbol(&so_mod, "_Z7appFreePv");

    int n = 0;
    n += ue3_hook("_ZN16UHorrorMenuScene18RenderScene_HorrorEP7UCanvasf", 0xE92D4070, 0xE59030BC, h_render0, (void **)&o_render[0]);
    n += ue3_hook("_ZN17UHorrorMenuPickup18RenderScene_HorrorEP7UCanvasf", 0xE92D4010, 0xED904A1B, h_render1, (void **)&o_render[1]);
    n += ue3_hook("_ZN23UHorrorMenuLibraryEntry18RenderScene_HorrorEP7UCanvasf", 0xE92D4010, 0xED904A1B, h_render2,
                  (void **)&o_render[2]);
    n += ue3_hook("_ZN24UHorrorMenuCombatSummary18RenderScene_HorrorEP7UCanvasf", 0xE59F3050, 0xE92D4070, h_render3,
                  (void **)&o_render[3]);
    n += ue3_hook("_ZN14UHorrorMenuHUD18RenderScene_HorrorEP7UCanvasf", 0xE92D4FF0, 0xE59F5200, h_render4, (void **)&o_render[4]);
    int states = ue3_hook("_ZN7UObject9GotoStateE5FNamejj", 0xE92D4FF0, 0xE1A08003, h_GotoState, (void **)&o_GotoState);
    int camera = ue3_hook("_ZN12ULocalPlayer13CalcSceneViewEP16FSceneViewFamilyR7FVectorR8FRotatorP9FViewportP18FViewElementDrawer",
                          0xE92D4FF0, 0xED2D8B10, h_CalcSceneView, (void **)&o_CalcSceneView);
    l_info("[ui] game state: %d of 5 screen functions hooked; state changes %s; camera %s", n,
           states ? "watched" : "NOT hooked", camera ? "watched" : "NOT hooked");
}
