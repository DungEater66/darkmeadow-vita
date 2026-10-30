#ifndef DM_WATCHDOG_H
#define DM_WATCHDOG_H

/* Start a low-priority thread that writes to log.txt when the engine stops presenting frames, and lists the
 * engine calls that have been blocked in a mutex/condition/semaphore/join for 5 s or more. */
void watchdog_start(void);

/* Include a loader thread in the periodic [cpu] report. */
void watchdog_register_thread(const char *name, int uid);

#endif
