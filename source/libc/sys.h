/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  sys.h
 * @brief Implementations and wrappers for misc. system functions.
 */

#ifndef SOLOADER_SYS_H
#define SOLOADER_SYS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/time.h>

#define PAGE_SIZE 4096




int __system_property_get_soloader(const char *name, char *value);


void syscall(int c);

void __stack_chk_fail_soloader();

void abort_soloader();

void exit_soloader(int status);

int __atomic_dec(volatile int *ptr);

int __atomic_inc(volatile int *ptr);

int __atomic_swap(int new_value, volatile int *ptr);

int __atomic_cmpxchg(int old_value, int new_value, volatile int* ptr);




#ifdef __cplusplus
};
#endif

#endif // SOLOADER_SYS_H
