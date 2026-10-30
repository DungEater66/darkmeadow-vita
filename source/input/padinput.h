/* Physical controls: the sticks and buttons as made-up touches (see padinput.c). */
#ifndef DM_PADINPUT_H
#define DM_PADINPUT_H

void padinput_init(void);
void padinput_poll(void);                 /* from the controls thread, ~120 times a second */
extern int padinput_log_touches;          /* DM_LOG_TOUCHES: log where real fingers go down */
extern volatile int padinput_profile;     /* 0 exploration, 1 sword fight, 2 menu, 3 crossbow fight, 4 cutscene, -1 off */
extern volatile float padinput_cursor_x, padinput_cursor_y;   /* the cursor, 960x544 */

/* Draw the cursor over the frame (exploration, menus, crossbow fights): at every buffer swap, on the rendering thread
 * (overlay.c). */
void overlay_render(void);

#endif
