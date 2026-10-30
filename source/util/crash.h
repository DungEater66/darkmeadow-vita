#ifndef DM_CRASH_H
#define DM_CRASH_H

/*
 * Install a crash (abort) handler through kubridge. When the engine or the loader faults, the handler writes the
 * registers, the faulting address and a rough call stack (as offsets into libUnrealEngine3.so) to log.txt and
 * exits. The offsets can be turned into C++ function names with tools/resolve_addr.py.
 */
void crash_init(void);

#endif
