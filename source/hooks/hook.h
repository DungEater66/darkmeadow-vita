/*
 * Hooking functions inside libUnrealEngine3.so, and the engine's string types.
 */
#ifndef DM_HOOK_H
#define DM_HOOK_H

#include <stddef.h>
#include <stdint.h>

/* Replace the ARM engine function `sym` with `dst`; *orig gets a trampoline that runs the original. The hook only
 * goes in if the function starts with exactly w0 and w1 (w1 = 0: not checked), so a different build of the library
 * is left alone. Returns 1 if installed. */
int ue3_hook(const char *sym, uint32_t w0, uint32_t w1, void *dst, void **orig);

/* The engine's TCHAR strings are UTF-32 (wchar_t is 4 bytes). */
void ue3_wstr_to_utf8(const uint32_t *w, char *out, size_t n);

/* An engine FString: { Data, ArrayNum, ArrayMax } */
typedef struct {
    uint32_t *data;
    int num, max;
} ue3_fstring_t;

/* Install every hook of the loader's own (from so_patch, before the engine's code caches are flushed). */
void hooks_install(void);

#endif
