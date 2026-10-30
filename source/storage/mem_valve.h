/* The engine's allocator: malloc and friends that make the loader's caches give memory back when the heap runs out,
 * counted for the [hitch] breakdown (see mem_valve.c). */
#ifndef DM_MEM_VALVE_H
#define DM_MEM_VALVE_H

#include <stddef.h>

void *malloc_valve(size_t n);
void free_valve(void *p);
void *realloc_valve(void *old, size_t n);

#endif
