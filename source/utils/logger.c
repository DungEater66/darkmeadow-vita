/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/logger.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

#define COLOR_RED    "\x1B[38;5;196m"
#define COLOR_PINK   "\x1B[38;5;212m"
#define COLOR_ORANGE "\x1B[38;5;202m"
#define COLOR_BLUE   "\x1B[38;5;32m"
#define COLOR_GREEN  "\x1B[32m"
#define COLOR_CYAN   "\x1B[36m"

#define COLOR_END    "\033[0m"

static SceKernelLwMutexWork _log_mutex;
static atomic_bool _log_mutex_ready = ATOMIC_VAR_INIT(false);

// Buffer A is used to adjust the format string.
static char buffer_a[2048];
// Buffer B is used to compile the final log using the updated format string.
static char buffer_b[2048];
// Buffer C holds the plain-text line written to the log file.
static char buffer_c[2048];

// Plain-text copy of everything that is logged, overwritten on every start: DATA_PATH "log.txt".
// -1 = not opened yet, -2 = could not be opened (do not retry).
//
// Writes are batched: lines collect in a 16 KB buffer that goes to the memory card when it is full, when a warning or
// worse is logged, or when a line arrives a second or more after the last write (the watchdog also calls log_flush()
// every second). Writing every line straight to the card, as before, made each log call wait for the card while
// holding the lock that every other logging thread needs. Must be called with the log lock held.
#define LOG_FILE DATA_PATH "log.txt"
static SceUID _log_fd = -1;
static char fbuf[16384];
static size_t fbuf_n;
static uint64_t last_flush_us;

static void flush_locked(void) {
    if (_log_fd == -2 || fbuf_n == 0) { fbuf_n = 0; return; }
    if (_log_fd == -1) {
        _log_fd = sceIoOpen(LOG_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (_log_fd < 0) { _log_fd = -2; fbuf_n = 0; return; }
    }
    sceIoWrite(_log_fd, fbuf, fbuf_n);
    fbuf_n = 0;
    last_flush_us = sceKernelGetProcessTimeWide();
}

static void log_file_write(const char *text, size_t len, int urgent) {
    if (_log_fd == -2) return;
    if (fbuf_n + len > sizeof(fbuf)) flush_locked();
    if (len > sizeof(fbuf)) len = sizeof(fbuf);
    memcpy(fbuf + fbuf_n, text, len);
    fbuf_n += len;
    if (urgent || sceKernelGetProcessTimeWide() - last_flush_us > 1000 * 1000) flush_locked();
}

static void ensure_mutex(void) {
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_acquire)) {
        int ret = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 0, 0, NULL);
        if (ret < 0) {
            sceClibPrintf("Error: failed to create log mutex: 0x%x\n", ret);
            return;
        }
        atomic_store_explicit(&_log_mutex_ready, true, memory_order_release);
    }
}

/* Lock for logging. Errors wait at most 0.2 s: the crash handler logs with l_fatal, and the crashed thread may have
 * died holding the lock. Returns whether the lock is held. */
static int log_lock(int t) {
    ensure_mutex();
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_acquire)) return 0;
    if (t >= LT_ERROR && t != LT_SUCCESS && t != LT_WAIT) {
        SceUInt timeout = 200 * 1000;
        return sceKernelLockLwMutex(&_log_mutex, 1, &timeout) >= 0;
    }
    return sceKernelLockLwMutex(&_log_mutex, 1, NULL) >= 0;
}

static void log_unlock(int held) {
    if (held) sceKernelUnlockLwMutex(&_log_mutex, 1);
}

void log_flush(void) {
    int held = log_lock(LT_DEBUG);
    flush_locked();
    log_unlock(held);
}

static const char *log_label(int t) {
    switch (t) {
        case LT_DEBUG:   return "[debug]";
        case LT_INFO:    return "[info ]";
        case LT_WARN:    return "[warn ]";
        case LT_ERROR:   return "[ERROR]";
        case LT_FATAL:   return "[FATAL]";
        case LT_SUCCESS: return "[ ok  ]";
        case LT_WAIT:    return "[wait ]";
        default:         return "[?????]";
    }
}

// For other loggers (FalsoJNI, vitaGL): append already formatted text, minus ANSI colour codes.
void log_write_raw(const char *text) {
    int held = log_lock(LT_DEBUG);
    size_t n = 0;
    for (const char *p = text; *p && n < sizeof(buffer_c) - 1; p++) {
        if (*p == 0x1B && p[1] == '[') {          // skip ESC [ ... m
            p += 2;
            while (*p && *p != 'm') p++;
            if (!*p) break;
            continue;
        }
        buffer_c[n++] = *p;
    }
    buffer_c[n] = 0;
    log_file_write(buffer_c, n, 0);
    log_unlock(held);
}

void _log_print(int t, const char* fmt, ...) {
    int held = log_lock(t);

    switch (t) {
        case LT_DEBUG:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s• debug%s    %s\n",
                            COLOR_PINK, COLOR_END, fmt); break;
        case LT_INFO:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %sℹ info%s     %s\n",
                            COLOR_BLUE, COLOR_END, fmt); break;
        case LT_WARN:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⚠ warning%s  %s\n",
                            COLOR_ORANGE, COLOR_END, fmt); break;
        case LT_ERROR:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⨯ error%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_FATAL:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! fatal%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_SUCCESS:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! success%s  %s\n",
                            COLOR_GREEN, COLOR_END, fmt); break;
        case LT_WAIT:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s… waiting%s  %s\n",
                            COLOR_CYAN, COLOR_END, fmt); break;
        default:
            log_unlock(held);
            return;
    }

    va_list list, list_file;
    va_start(list, fmt);
    va_copy(list_file, list);
    sceClibVsnprintf(buffer_b, sizeof(buffer_b), buffer_a, list);
    va_end(list);
    sceClibPrintf(buffer_b);

    /* r68: every line carries the time since start, in seconds with milliseconds */
    uint64_t now_ms = sceKernelGetProcessTimeWide() / 1000;
    int n = sceClibSnprintf(buffer_c, sizeof(buffer_c), "%s %5u.%03u ", log_label(t), (unsigned)(now_ms / 1000), (unsigned)(now_ms % 1000));
    int m = sceClibVsnprintf(buffer_c + n, sizeof(buffer_c) - n - 2, fmt, list_file);
    va_end(list_file);
    if (m < 0) m = 0;
    if (m > (int)(sizeof(buffer_c) - n - 2)) m = sizeof(buffer_c) - n - 2;   // truncated
    buffer_c[n + m] = '\n';
    log_file_write(buffer_c, n + m + 1, t == LT_WARN || t == LT_ERROR || t == LT_FATAL);

    log_unlock(held);
}

// vitaGL's log sink (see lib/vitagl/source/utils/debug_utils.h): console plus log.txt.
void dm_vgl_log(const char *fmt, ...) {
    static char vgl_buf[1024];
    va_list ap;
    va_start(ap, fmt);
    sceClibVsnprintf(vgl_buf, sizeof(vgl_buf), fmt, ap);
    va_end(ap);
    sceClibPrintf("%s", vgl_buf);
    log_write_raw(vgl_buf);
}
