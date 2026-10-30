#ifndef DM_UE3_TRACE_H
#define DM_UE3_TRACE_H

/*
 * Pass-through loggers for a few engine functions (written to log.txt in Debug builds):
 *   unzOpen2(path)               did the engine's zip reader open the .obb files?
 *   FFileManagerAndroid::FileSize / FindFiles   which paths and patterns does the file manager look up, and with what result?
 * Each hook is installed only if the first two instructions of the function are exactly the expected ones, so an
 * unexpected build of the library is left untouched.
 */
void ue3_trace_install(void);

#include <stdint.h>
/* Hook the ARM engine function `sym` (first two instructions w0, w1; w1 = 0 = not checked) with `dst`; *orig gets a
 * trampoline that runs the original. Returns 1 if installed. */
int ue3_hook(const char *sym, uint32_t w0, uint32_t w1, void *dst, void **orig);

#endif
