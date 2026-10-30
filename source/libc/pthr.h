/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022      GrapheneCt
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  pthr.h
 * @brief Wrapper for vitasdk/newlib pthread functions to work with
 *        Android's pthread structs which are different
 */

#ifndef SOLOADER_PTHR_H
#define SOLOADER_PTHR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>

/* A thread that has been blocked in a pthread/semaphore call (see pthr_collect_stalls). */
typedef struct {
    const char *what;      /* e.g. "pthread_cond_wait" */
    const void *obj;       /* the engine's mutex/cond/semaphore/thread */
    uint64_t blocked_us;   /* how long it has been waiting */
    uintptr_t caller;      /* return address into the engine */
} pthr_stall_t;

/* Fill `out` with up to `max` calls that have been blocked for at least `min_us`; returns how many. */
int pthr_collect_stalls(uint64_t min_us, pthr_stall_t *out, int max);

/* Call sites that keep polling (timed waits, failed trylocks, usleep, sched_yield), counted since the last call. */
typedef struct {
    const char *what;
    uintptr_t caller;
    unsigned count;
} pthr_busy_t;

void pthr_note(const char *what, void *caller);
int pthr_collect_busy(pthr_busy_t *out, int max);

int usleep_soloader(unsigned int usec);
int cxa_guard_acquire_tracked(int *guard);
int sched_yield_soloader(void);

/* A thread the engine created (pthread_create). */
typedef struct {
    int tid;              /* kernel thread id */
    uintptr_t start;      /* entry point in the engine */
    void *param;
} pthr_thread_t;

int pthr_list_threads(pthr_thread_t *out, int max);

/* Offset of an address inside libUnrealEngine3.so's code, or 0 if it is not in it. */
uintptr_t pthr_engine_offset(uintptr_t addr);

typedef struct {
    pthread_attr_t *real_ptr; // replaces `uint32_t flags;`
    int32_t magic; // replaces `void * stack_base;`
    size_t stack_size;
    size_t guard_size;
    int32_t sched_policy;
    int32_t sched_priority;
} pthread_attr_t_bionic;

typedef struct {
    pthread_mutex_t *real_ptr; // replaces `int volatile value;`
} pthread_mutex_t_bionic;

typedef struct {
    pthread_cond_t *real_ptr; // replaces `int volatile value;`
} pthread_cond_t_bionic;

// pthread_t is same size on bionic and newlib
int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr, void *(*start)(void *), void *param);
int pthread_join_soloader(pthread_t thread, void **value_ptr);
int pthread_detach_soloader(pthread_t thread);
pthread_t pthread_self_soloader();
int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void));

// pthread_t and sched_param are same size on bionic and newlib

// condattr_t is same size on bionic and newlib

// mutexattr_t is same size on bionic and newlib
int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr);
int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type);
int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr);

int pthread_cond_init_soloader(pthread_cond_t_bionic *cond, const pthread_condattr_t *attr);
int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond);
int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex);
int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex, struct timespec *abstime);
int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond);
int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond);

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex);
int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex);
int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex);
int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex);

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr);
int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize);



#ifdef __cplusplus
};
#endif

#endif // SOLOADER_PTHR_H
