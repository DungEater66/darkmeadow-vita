/* The watchdog thread: freeze diagnostics, the periodic [cpu] / [gpu] / [mem] lines and thread pinning (watchdog.c). */
#ifndef DM_WATCHDOG_H
#define DM_WATCHDOG_H

void watchdog_start(void);

/* Include a loader thread in the [cpu] lines (and in pinning). */
void watchdog_register_thread(const char *name, int uid);

/* The thread that swaps buffers (the engine's rendering thread), set at its first swap. */
extern volatile int dm_render_tid;

#endif
