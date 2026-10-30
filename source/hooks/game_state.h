/*
 * What the game is doing, read from its own functions (see game_state.c). Written on the engine's threads, read by
 * the physical controls and the movie player. Times are sceKernelGetProcessTimeWide() microseconds, 0 = never.
 */
#ifndef DM_GAME_STATE_H
#define DM_GAME_STATE_H

#include <stdint.h>

enum { FIGHT_NONE = 0, FIGHT_SWORD = 1, FIGHT_CROSSBOW = 2 };

typedef struct {
    uint64_t hud_drawn_us;         /* last time an in-game screen (HUD, pause button, popup text...) was drawn */
    uint64_t menu_drawn_us;        /* last time any other screen (menus, store, journal, fight summary) was drawn */
    char menu_name[64];            /* class of that screen, e.g. HorrorMenuPause */
    int fight;                     /* FIGHT_* */
    int fight_seen;                /* the player's fight state has been seen at least once (the hook works) */
    uint64_t fight_end_us;         /* when the last fight ended */
    int post_fight;                /* the player controller is in PostCombatTransition */
    uint64_t post_fight_exit_us;   /* when it left it */
    uint64_t camera_moved_us;      /* last frame the camera turned */
} game_state_t;

extern volatile game_state_t game_state;

#endif
