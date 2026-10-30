/*
 * Small helpers. Based on the so-loader template's utils (Copyright (C) 2021 Rinnegatamante, 2022-2024 Volodymyr
 * Atamanenko; MIT license, see LICENSE).
 */
#include "util/utils.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <psp2/io/stat.h>

#include "util/logger.h"

bool file_exists(const char *path) {
    SceIoStat stat;
    return sceIoGetstat(path, &stat) >= 0;
}

bool file_mkpath(const char *path, mode_t mode) {
    if (!path || !*path) return false;
    char *p_path = strdup(path);
    if (!p_path) return false;
    for (char *p = strchr(p_path + 1, '/'); p; p = strchr(p + 1, '/')) {
        *p = '\0';
        if (mkdir(p_path, mode) == -1 && errno != EEXIST) {
            l_error("file_mkpath: cannot create \"%s\": %s", p_path, strerror(errno));
            free(p_path);
            return false;
        }
        *p = '/';
    }
    free(p_path);
    return true;
}

SceUID _vshKernelSearchModuleByName(const char *, int *);

bool module_loaded(const char *name) {
    int search_unk[2];
    return _vshKernelSearchModuleByName(name, search_unk) >= 0;
}

int ret0(void) { return 0; }
int ret1(void) { return 1; }
