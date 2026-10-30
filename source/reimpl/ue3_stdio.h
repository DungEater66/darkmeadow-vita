#ifndef DM_UE3_STDIO_H
#define DM_UE3_STDIO_H

/*
 * libUnrealEngine3.so contains its own copy of Android's (BSD) C stdio: fclose, fread, fseek, ftell, fgets, ...
 * but gets fopen() from the system libc. On Android both use the same FILE layout; here fopen() returns a newlib FILE
 * and the engine's internal functions would follow bionic's FILE internals into garbage (crash in _fseeko).
 *
 * This hooks the engine's internal stdio functions and forwards them to the Vita's own stdio. The engine's own
 * stdin/stdout/stderr objects (its internal __sF array) are handled separately.
 */
void ue3_stdio_hook_all(void);

#endif
