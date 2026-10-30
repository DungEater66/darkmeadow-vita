/*
 * A fatal error: show the message in a system dialog and quit.
 */
#ifndef DM_FATAL_H
#define DM_FATAL_H

#ifdef __cplusplus
extern "C" {
#endif

void fatal_error(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif
