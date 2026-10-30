/*
 * Small helpers. Based on the so-loader template's utils (Rinnegatamante, Volodymyr Atamanenko; MIT).
 */
#ifndef DM_UTILS_H
#define DM_UTILS_H

#include <stdbool.h>
#include <sys/types.h>

bool file_exists(const char *path);
bool file_mkpath(const char *path, mode_t mode);   /* create the parent directories of `path` */
bool module_loaded(const char *name);              /* a kernel/user module is loaded */

/* stand-ins for engine imports that only need to return a value */
int ret0(void);
int ret1(void);

#endif
