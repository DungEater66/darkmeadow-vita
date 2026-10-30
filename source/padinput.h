#ifndef DM_PADINPUT_H
#define DM_PADINPUT_H

/* r59/r60: sticks and buttons as made-up touches, in four profiles (see padinput.c) */
void padinput_init(void);
void padinput_poll(void);                 /* from the controls thread, ~120 times a second */
extern int padinput_log_touches;          /* DM_LOG_TOUCHES */
extern volatile int padinput_profile;     /* 0 exploration, 1 sword fight, 2 menu, 3 crossbow fight, -1 off */
extern volatile float padinput_cursor_x, padinput_cursor_y;   /* cursor, 960x544 */

/* the cursor (exploration, menus, crossbow fights), drawn over the frame at every buffer swap (overlay.c, rendering thread) */
void overlay_render(void);

#endif
