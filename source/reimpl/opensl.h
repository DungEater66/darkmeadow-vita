#ifndef DM_OPENSL_H
#define DM_OPENSL_H

#include <stdint.h>

/* The loader's own OpenSL ES subset (see opensl.c). dynlib.c hands these to the engine instead of libOpenSLES. */
uint32_t dm_slCreateEngine(void *pEngine, uint32_t numOptions, const void *options, uint32_t numInterfaces,
                           const void *interfaceIds, const uint32_t *interfaceRequired);
extern const void *const dm_SL_IID_ENGINE;
extern const void *const dm_SL_IID_PLAY;
extern const void *const dm_SL_IID_VOLUME;
extern const void *const dm_SL_IID_BUFFERQUEUE;
extern const void *const dm_SL_IID_ANDROIDSIMPLEBUFFERQUEUE;

void opensl_init(void);
void opensl_report(void);

#endif
