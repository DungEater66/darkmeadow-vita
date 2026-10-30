/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022      GrapheneCt
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "libc/pthr.h"

#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <stdatomic.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "util/utils.h"
#include "util/logger.h"

/*
 * Android (bionic) pthread_mutex_t / pthread_cond_t are 4 bytes; the engine either fills them with a static
 * initializer (0, or 0x4000 / 0x8000 for recursive / error-checking mutexes) or calls *_init on them.
 * We keep a pointer to a real newlib object in those 4 bytes and create it on first use.
 *
 * The boilerplate remembered every initialized object in a fixed table of 1024 entries. Unreal Engine 3 creates
 * far more sync objects than that; once the table was full, every lock/wait/signal created a brand-new object,
 * so a waiter and its signaller never met and threads slept forever (no crash, no disk access).
 * Here the "is it created yet" test is simply "is the stored value a pointer", done with an atomic swap.
 */
#define BIONIC_PTHREAD_COND_INITIALIZER              0
#define BIONIC_PTHREAD_MUTEX_INITIALIZER             0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER   0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER  0x8000

#define PTHR_INLINE static inline __attribute__((always_inline))
#define PTHR_IS_REAL(p) ((uintptr_t)(p) >= 0x10000u)

extern so_module so_mod;

// null check for `attr` must be performed before this
PTHR_INLINE int _attr_t_static_init(pthread_attr_t_bionic * attr) {
    if (attr->magic != 0x42424242) {
        attr->magic = 0x42424242;
        attr->real_ptr = malloc(sizeof(pthread_attr_t));
        return pthread_attr_init(attr->real_ptr);
    }
    return 0;
}

static pthread_mutex_t *mutex_create(int kind) {
    pthread_mutex_t *m = malloc(sizeof(pthread_mutex_t));
    if (!m) return NULL;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, kind);
    int ret = pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    if (ret != 0) {
        l_error("pthread_mutex_init failed: %d", ret);
        free(m);
        return NULL;
    }
    return m;
}

static pthread_cond_t *cond_create(const pthread_condattr_t *attr) {
    pthread_cond_t *c = malloc(sizeof(pthread_cond_t));
    if (!c) return NULL;
    int ret = pthread_cond_init(c, attr);
    if (ret != 0) {
        l_error("pthread_cond_init failed: %d", ret);
        free(c);
        return NULL;
    }
    return c;
}

static int bionic_kind(uintptr_t v) {
    if (v == BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER) return PTHREAD_MUTEX_RECURSIVE;
    if (v == BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER) return PTHREAD_MUTEX_ERRORCHECK;
    return PTHREAD_MUTEX_NORMAL;
}

/* The real mutex behind a bionic one, created on first use (the engine relies on static initializers). */
static pthread_mutex_t *mutex_get(pthread_mutex_t_bionic *mutex) {
    pthread_mutex_t *cur = mutex->real_ptr;
    if (PTHR_IS_REAL(cur)) return cur;
    pthread_mutex_t *m = mutex_create(bionic_kind((uintptr_t)cur));
    if (!m) return NULL;
    if (__sync_bool_compare_and_swap(&mutex->real_ptr, cur, m)) return m;
    pthread_mutex_destroy(m);   /* another thread got there first */
    free(m);
    return mutex->real_ptr;
}

static pthread_cond_t *cond_get(pthread_cond_t_bionic *cond) {
    pthread_cond_t *cur = cond->real_ptr;
    if (PTHR_IS_REAL(cur)) return cur;
    pthread_cond_t *c = cond_create(NULL);
    if (!c) return NULL;
    if (__sync_bool_compare_and_swap(&cond->real_ptr, cur, c)) return c;
    pthread_cond_destroy(c);
    free(c);
    return cond->real_ptr;
}

/* ---- stall tracking: which engine call has been blocked for how long (read by util/watchdog.c) ---- */

#define PTHR_WAIT_SLOTS 64
typedef struct {
    volatile int used;
    const char *what;
    const void *obj;
    uintptr_t caller;
    uint64_t since_us;
} wait_slot_t;
static wait_slot_t wait_slots[PTHR_WAIT_SLOTS];

static int wait_begin(const char *what, const void *obj, void *lr) {
    for (int i = 0; i < PTHR_WAIT_SLOTS; i++) {
        if (!wait_slots[i].used && __sync_bool_compare_and_swap(&wait_slots[i].used, 0, 2)) {
            wait_slots[i].what = what;
            wait_slots[i].obj = obj;
            wait_slots[i].caller = (uintptr_t)lr;
            wait_slots[i].since_us = sceKernelGetProcessTimeWide();
            __sync_synchronize();
            wait_slots[i].used = 1;
            return i;
        }
    }
    return -1;
}

static void wait_end(int i) {
    if (i >= 0) wait_slots[i].used = 0;
}

int pthr_collect_stalls(uint64_t min_us, pthr_stall_t *out, int max) {
    uint64_t now = sceKernelGetProcessTimeWide();
    int n = 0;
    for (int i = 0; i < PTHR_WAIT_SLOTS && n < max; i++) {
        if (wait_slots[i].used != 1) continue;
        wait_slot_t s = wait_slots[i];
        if (s.used != 1 || now < s.since_us || now - s.since_us < min_us) continue;
        out[n].what = s.what;
        out[n].obj = s.obj;
        out[n].blocked_us = now - s.since_us;
        out[n].caller = s.caller;
        n++;
    }
    return n;
}

/* ---- busy-wait tracking: short waits/yields that a thread repeats in a loop never show up as a stall, so count
 * them per call site; the watchdog prints the busiest ones while the engine is frozen. ---- */

#define PTHR_BUSY_SLOTS 64
typedef struct { const char *what; uintptr_t caller; volatile unsigned count; } busy_slot_t;
static busy_slot_t busy_slots[PTHR_BUSY_SLOTS];

void pthr_note(const char *what, void *lr) {
    uintptr_t c = (uintptr_t)lr;
    unsigned h = (unsigned)((c >> 2) ^ (c >> 9)) % PTHR_BUSY_SLOTS;
    for (int k = 0; k < PTHR_BUSY_SLOTS; k++) {
        busy_slot_t *b = &busy_slots[(h + k) % PTHR_BUSY_SLOTS];
        if (b->caller == c) { __sync_fetch_and_add(&b->count, 1); return; }
        if (b->caller == 0 && __sync_bool_compare_and_swap(&b->caller, 0, c)) {
            b->what = what;
            __sync_fetch_and_add(&b->count, 1);
            return;
        }
    }
}

int pthr_collect_busy(pthr_busy_t *out, int max) {
    int n = 0;
    for (int i = 0; i < PTHR_BUSY_SLOTS; i++) {
        unsigned c = __sync_lock_test_and_set(&busy_slots[i].count, 0);
        if (!c || !busy_slots[i].what) continue;
        if (n < max) {
            out[n].what = busy_slots[i].what;
            out[n].caller = busy_slots[i].caller;
            out[n].count = c;
            n++;
        } else {   /* keep the busiest */
            int lo = 0;
            for (int j = 1; j < max; j++) if (out[j].count < out[lo].count) lo = j;
            if (out[lo].count < c) { out[lo].what = busy_slots[i].what; out[lo].caller = busy_slots[i].caller; out[lo].count = c; }
        }
    }
    return n;
}

/* C++ function-local statics: libstdc++'s guard waits on a global condition variable when another thread is
 * initializing the same static. Track it like the other waits so a stuck static initializer shows up as a [stall]. */
extern int __cxa_guard_acquire(int *guard);

int cxa_guard_acquire_tracked(int *guard) {
    if (*(volatile int *)guard & 1) return 0;   /* already initialized */
    int slot = wait_begin("__cxa_guard_acquire", guard, __builtin_return_address(0));
    int r = __cxa_guard_acquire(guard);
    wait_end(slot);
    return r;
}

int usleep_soloader(unsigned int usec) {
    pthr_note("usleep", __builtin_return_address(0));
    return usleep(usec);
}

int sched_yield_soloader(void) {
    pthr_note("sched_yield", __builtin_return_address(0));
    /* newlib's sched_yield does not let lower-priority threads run on the Vita; really give up the CPU */
    sceKernelDelayThread(100);
    return 0;
}

uintptr_t pthr_engine_offset(uintptr_t a) {
    if (a >= so_mod.text_base && a < so_mod.text_base + so_mod.text_size) return a - so_mod.text_base;
    return 0;
}

/* Timed waits take an absolute CLOCK_REALTIME deadline. Warn (once) if the engine computes it on another clock,
 * which would turn a short timed wait into a very long one. */
static void check_abstime(const struct timespec *abstime, void *lr) {
    static int warned;
    if (!abstime || warned) return;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long d = (long long)abstime->tv_sec - (long long)tv.tv_sec;
    if (d > 60 || d < -60) {
        warned = 1;
        l_warn("[pthr] timed wait deadline is %lld s away from the wall clock (caller libUnrealEngine3.so+0x%x)",
               d, (unsigned)pthr_engine_offset((uintptr_t)lr));
    }
}

/* ---- engine threads: remember each one's kernel thread id and entry point, so the watchdog can show what every
 * thread is doing (running / waiting, and how much CPU it used) while the game is frozen. ---- */

#define PTHR_MAX_THREADS 64
typedef struct { void *(*start)(void *); void *param; } thread_boot_t;
static pthr_thread_t threads[PTHR_MAX_THREADS];
static volatile int threads_n;

static void *thread_trampoline(void *arg) {
    thread_boot_t b = *(thread_boot_t *)arg;
    free(arg);
    int i = __sync_fetch_and_add(&threads_n, 1);
    if (i < PTHR_MAX_THREADS) {
        threads[i].start = (uintptr_t)b.start;
        threads[i].param = b.param;
        threads[i].tid = sceKernelGetThreadId();
    }
    return b.start(b.param);
}

int pthr_list_threads(pthr_thread_t *out, int max) {
    int n = threads_n < PTHR_MAX_THREADS ? threads_n : PTHR_MAX_THREADS;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = threads[i];
    return n;
}

/* Bionic's default stack is 1 MiB and the engine asks for its own sizes (PhysX: 1 MiB). The boilerplate forced every
 * thread to 512 KiB; keep whatever the engine asked for, with 512 KiB as the floor. */
#define PTHR_MIN_STACK (512 * 1024)
#define PTHR_MAX_STACK (4 * 1024 * 1024)

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr, void *(*start)(void *), void *param) {
    int ret;
    thread_boot_t *boot = malloc(sizeof(*boot));
    if (!boot) return EAGAIN;
    boot->start = start;
    boot->param = param;

    if (!attr) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 1024 * 1024);
        ret = pthread_create(thread, &a, thread_trampoline, boot);
        pthread_attr_destroy(&a);
    } else {
        _attr_t_static_init((pthread_attr_t_bionic *) attr);
        size_t sz = 0;
        pthread_attr_getstacksize(attr->real_ptr, &sz);
        if (sz < PTHR_MIN_STACK) pthread_attr_setstacksize(attr->real_ptr, PTHR_MIN_STACK);
        else if (sz > PTHR_MAX_STACK) pthread_attr_setstacksize(attr->real_ptr, PTHR_MAX_STACK);
        ret = pthread_create(thread, attr->real_ptr, thread_trampoline, boot);
    }
    if (ret != 0) {
        l_error("pthread_create failed: %d (entry libUnrealEngine3.so+0x%x)", ret, (unsigned)pthr_engine_offset((uintptr_t)start));
        free(boot);
    }
    return ret;
}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_init(attr);
}

int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type)
{
    return pthread_mutexattr_settype(attr, type);
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_destroy(attr);
}

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr)
{
    if (!uid) return EINVAL;
    int kind = PTHREAD_MUTEX_NORMAL;
    if (attr) pthread_mutexattr_gettype((pthread_mutexattr_t *)attr, &kind);
    /* the memory is uninitialized here: never trust what it holds */
    pthread_mutex_t *m = mutex_create(kind);
    if (!m) return ENOMEM;
    uid->real_ptr = m;
    return 0;
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return 0;
    pthread_mutex_t *m = mutex->real_ptr;
    mutex->real_ptr = NULL;
    if (!PTHR_IS_REAL(m)) return 0;
    int ret = pthread_mutex_destroy(m);
    free(m);
    return ret;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    pthread_mutex_t *m = mutex_get(mutex);
    if (!m) return EINVAL;
    if (pthread_mutex_trylock(m) == 0) return 0;
    int slot = wait_begin("pthread_mutex_lock", mutex, __builtin_return_address(0));
    int ret = pthread_mutex_lock(m);
    wait_end(slot);
    return ret;
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    pthread_mutex_t *m = mutex_get(mutex);
    if (!m) return EINVAL;
    int ret = pthread_mutex_trylock(m);
    if (ret != 0) pthr_note("pthread_mutex_trylock (busy)", __builtin_return_address(0));
    return ret;
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    pthread_mutex_t *m = mutex->real_ptr;
    if (!PTHR_IS_REAL(m)) return EPERM;
    return pthread_mutex_unlock(m);
}

int pthread_join_soloader(pthread_t thread, void **value_ptr)
{
    int slot = wait_begin("pthread_join", (const void *)thread, __builtin_return_address(0));
    int ret = pthread_join(thread, value_ptr);
    wait_end(slot);
    return ret;
}

int pthread_cond_init_soloader(pthread_cond_t_bionic *cond,
                               const pthread_condattr_t *attr)
{
    if (!cond) return EINVAL;
    pthread_cond_t *c = cond_create(attr);
    if (!c) return ENOMEM;
    cond->real_ptr = c;
    return 0;
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return 0;
    pthread_cond_t *c = cond->real_ptr;
    cond->real_ptr = NULL;
    if (!PTHR_IS_REAL(c)) return 0;
    int ret = pthread_cond_destroy(c);
    free(c);
    return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;
    pthread_cond_t *c = cond_get(cond);
    return c ? pthread_cond_signal(c) : EINVAL;
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;
    pthread_cond_t *c = cond_get(cond);
    return c ? pthread_cond_broadcast(c) : EINVAL;
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex, struct timespec *abstime)
{
    if (!cond || !mutex) return EINVAL;
    pthread_cond_t *c = cond_get(cond);
    pthread_mutex_t *m = mutex_get(mutex);
    if (!c || !m) return EINVAL;
    void *lr = __builtin_return_address(0);
    check_abstime(abstime, lr);
    pthr_note("pthread_cond_timedwait", lr);
    int slot = wait_begin("pthread_cond_timedwait", cond, lr);
    int ret = pthread_cond_timedwait(c, m, abstime);
    wait_end(slot);
    return ret;
}

int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex)
{
    if (!cond || !mutex) return EINVAL;
    pthread_cond_t *c = cond_get(cond);
    pthread_mutex_t *m = mutex_get(mutex);
    if (!c || !m) return EINVAL;
    int slot = wait_begin("pthread_cond_wait", cond, __builtin_return_address(0));
    int ret = pthread_cond_wait(c, m);
    wait_end(slot);
    return ret;
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return EINVAL;
    attr->magic = 0;   /* uninitialized stack memory: never trust a leftover magic */
    return _attr_t_static_init(attr);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
    if (!attr) return -1;
    _attr_t_static_init(attr);
    return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}

int pthread_detach_soloader(pthread_t thread)
{
    return pthread_detach(thread);
}

pthread_t pthread_self_soloader()
{
    return pthread_self();
}

int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine)
        return -1;
    if (__sync_lock_test_and_set(once_control, 1) == 0)
        (*init_routine)();
    return 0;
}

