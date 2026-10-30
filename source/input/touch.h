/* The front touch screen, passed to the engine as Android touch events (see touch.c). */
#ifndef DM_TOUCH_H
#define DM_TOUCH_H

void touch_init(void);
void touch_poll(void);   /* from the controls thread, ~120 times a second */

#endif
