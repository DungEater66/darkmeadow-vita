/* The hook groups, installed together by hooks_install() (hooks/hook.c). */
#ifndef DM_HOOKS_INTERNAL_H
#define DM_HOOKS_INTERNAL_H

void engine_log_install(void);      /* engine_log.c: the engine's log into log.txt */
void load_profiler_install(void);   /* load_profiler.c: load and clean-up timing, file requests */
void text_scale_install(void);      /* text_scale.c: smaller wrapped text and subtitles */
void game_state_install(void);      /* game_state.c: screens, fights, camera */

#endif
