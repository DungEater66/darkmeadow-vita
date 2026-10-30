/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  io.h
 * @brief Wrappers and implementations for some of the IO functions.
 */

#ifndef SOLOADER_IO_H
#define SOLOADER_IO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdint.h>
#include <sys/dirent.h>
#include <sys/syslimits.h>
#include <sys/fcntl.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#ifndef DT_DIR
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#define DT_WHT 14
#endif

/* Bionic's 32-bit ARM `struct stat` (= stat64), with its natural alignment: 104 bytes, st_size at offset 48.
 * The boilerplate declared it packed and with newlib's 16-bit nlink_t/uid_t/gid_t, which put st_size at offset 38:
 * the engine read garbage file sizes from stat()/fstat() (FFileManagerAndroid reads offsets 0x10 and 0x30) and,
 * among other things, could not load its save game. */
typedef struct stat64_bionic {
    unsigned long long st_dev;      /*  0 */
    unsigned char __pad0[4];        /*  8 */
    uint32_t __st_ino;              /* 12 */
    uint32_t st_mode;               /* 16 */
    uint32_t st_nlink;              /* 20 */
    uint32_t st_uid;                /* 24 */
    uint32_t st_gid;                /* 28 */
    unsigned long long st_rdev;     /* 32 */
    unsigned char __pad3[4];        /* 40 (+4 alignment padding) */
    long long st_size;              /* 48 */
    uint32_t st_blksize;            /* 56 (+4 alignment padding) */
    unsigned long long st_blocks;   /* 64 */
    struct { int32_t tv_sec; int32_t tv_nsec; } st_atim, st_mtim, st_ctim;   /* 72, 80, 88 */
    unsigned long long st_ino;      /* 96 */
} stat64_bionic;
#ifdef __cplusplus
static_assert(__builtin_offsetof(stat64_bionic, st_size) == 48, "bionic stat layout");
static_assert(sizeof(stat64_bionic) == 104, "bionic stat size");
#else
_Static_assert(__builtin_offsetof(stat64_bionic, st_size) == 48, "bionic stat layout");
_Static_assert(sizeof(stat64_bionic) == 104, "bionic stat size");
#endif

typedef struct __attribute__((__packed__)) dirent64_bionic {
    int16_t d_ino; // 2 bytes // offset 0x0
    int64_t d_off; // 8 bytes // offset 0x2
    uint64_t d_reclen; // 8 bytes // 0xA
    unsigned char d_type; // 1 byte // offset 0x12
    char d_name[256]; // 256 bytes // offset 0x13
} dirent64_bionic;

int open_soloader(const char * path, int oflag, ...);

FILE * fopen_soloader(const char * filename, const char * mode);

DIR *opendir_soloader(char *name);

int stat_soloader(const char * path, stat64_bionic * buf);

int fstat_soloader(int fd, stat64_bionic * buf);

struct dirent64_bionic * readdir_soloader(DIR *dir);


int close_soloader(int fd);

int fclose_soloader(FILE *f);

int closedir_soloader(DIR *dir);
int dup_soloader(int fd);                          
int fcntl_soloader(int fd, int cmd, ...);

int ioctl_soloader(int fd, int request, ... /* arg */);


ssize_t write_soloader(int fd, const void *buf, size_t n);
int rename_soloader(const char *from, const char *to);

int remove_soloader(const char *path);
int unlink_soloader(const char *path);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_IO_H
