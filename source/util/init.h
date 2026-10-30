/* Start-up (init.c), and the two halves of loading the .so that live elsewhere. */
#ifndef DM_INIT_H
#define DM_INIT_H

#include <so_util/so_util.h>

void soloader_init_all(void);          /* load, relocate, patch and start libUnrealEngine3.so; GL, FalsoJNI, touch */
void resolve_imports(so_module *mod);  /* android/dynlib.c: the engine's imports */
void so_patch(void);                   /* android/patch.c: the loader's hooks into the engine */

#endif
