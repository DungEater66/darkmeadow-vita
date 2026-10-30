/*
 * The engine's own log, copied into log.txt.
 *
 * This is a shipping build: appOutputDebugString does nothing, so everything the engine logs is lost.
 * FOutputDevice::Logf is hooked to copy each message as [UE3] and then hand the formatted text on to the original.
 * The first 600 messages are kept; a message repeated straight away is counted instead of written again.
 * Settings app: "Engine log" (DM_UE3_LOG).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "hooks/hook.h"
#include "hooks/hooks_internal.h"
#include "util/logger.h"

#define MAX_MESSAGES 600

static int enabled = 1;

/* Format a UE3 TCHAR format string: wchar_t is 4 bytes and %s takes a TCHAR string. */
static void format_tchar(char *out, size_t n, const uint32_t *f, va_list ap) {
    size_t o = 0;
#define PUT(c) do { if (o + 1 < n) out[o++] = (char)(c); } while (0)
    for (; f && *f && o + 1 < n; f++) {
        if (*f != '%') {
            PUT(*f < 0x80 ? *f : '?');
            continue;
        }
        char spec[16];
        int sl = 0;
        spec[sl++] = '%';
        f++;
        while (*f && strchr("-+ #0123456789.*", (int)*f) && sl < 10) {
            if (*f == '*') {
                sl += snprintf(spec + sl, sizeof(spec) - sl, "%d", va_arg(ap, int));
                f++;
                continue;
            }
            spec[sl++] = (char)*f++;
        }
        int longs = 0;
        while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'I') {
            if (*f == 'l') longs++;
            f++;
        }
        uint32_t c = *f;
        if (!c) break;
        char tmp[256];
        tmp[0] = 0;
        switch (c) {
        case '%':
            PUT('%');
            continue;
        case 's': case 'S': {
            static const uint32_t null_str[] = { '(', 'n', 'u', 'l', 'l', ')', 0 };
            const uint32_t *ws = va_arg(ap, const uint32_t *);
            char u[256];
            ue3_wstr_to_utf8(ws ? ws : null_str, u, sizeof(u));
            spec[sl++] = 's';
            spec[sl] = 0;
            snprintf(tmp, sizeof(tmp), spec, u);
            break;
        }
        case 'c': {
            uint32_t ch = (uint32_t)va_arg(ap, int);
            tmp[0] = ch < 0x80 ? (char)ch : '?';
            tmp[1] = 0;
            break;
        }
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
            if (longs >= 2) {
                spec[sl++] = 'l';
                spec[sl++] = 'l';
                spec[sl++] = (char)c;
                spec[sl] = 0;
                snprintf(tmp, sizeof(tmp), spec, va_arg(ap, long long));
            } else {
                spec[sl++] = (char)c;
                spec[sl] = 0;
                snprintf(tmp, sizeof(tmp), spec, va_arg(ap, int));
            }
            break;
        case 'f': case 'F': case 'g': case 'G': case 'e': case 'E':
            spec[sl++] = (char)c;
            spec[sl] = 0;
            snprintf(tmp, sizeof(tmp), spec, va_arg(ap, double));
            break;
        case 'p':
            snprintf(tmp, sizeof(tmp), "%p", va_arg(ap, void *));
            break;
        default:
            PUT('%');
            PUT(c < 0x80 ? c : '?');
            continue;
        }
        for (char *t = tmp; *t; t++) PUT(*t);
    }
    out[o] = 0;
#undef PUT
}

static void log_message(int type, const char *text) {
    static unsigned count, last_hash, repeats;
    unsigned h = 5381;
    for (const char *t = text; *t; t++) h = h * 33 + (unsigned char)*t;
    if (h == last_hash) {
        repeats++;
        return;
    }
    if (repeats && count < MAX_MESSAGES) l_info("[UE3] (previous message repeated %u more times)", repeats);
    repeats = 0;
    last_hash = h;
    if (count++ >= MAX_MESSAGES) {
        if (count == MAX_MESSAGES + 1) l_info("[UE3] (%d messages logged; the rest are not)", MAX_MESSAGES);
        return;
    }
    if (type >= 0) l_info("[UE3] (%d) %s", type, text);
    else l_info("[UE3] %s", text);
}

/* the formatted text goes on to the original as "%s", text */
static const uint32_t percent_s[] = { '%', 's', 0 };

static void widen(const char *text, uint32_t *w, size_t n) {
    size_t i = 0;
    for (; text[i] && i + 1 < n; i++) w[i] = (unsigned char)text[i];
    w[i] = 0;
}

static void (*o_Logf)(void *, const uint32_t *, ...);
static void h_Logf(void *self, const uint32_t *fmt, ...) {
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    format_tchar(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (enabled) log_message(-1, text);
    uint32_t w[1024];
    widen(text, w, 1024);
    o_Logf(self, percent_s, w);
}

static void (*o_LogfType)(void *, int, const uint32_t *, ...);
static void h_LogfType(void *self, int type, const uint32_t *fmt, ...) {
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    format_tchar(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (enabled) log_message(type, text);
    uint32_t w[1024];
    widen(text, w, 1024);
    o_LogfType(self, type, percent_s, w);
}

void engine_log_install(void) {
    enabled = config_bool("DM_UE3_LOG", 1);
    int n = 0;
    n += ue3_hook("_ZN13FOutputDevice4LogfEPKwz", 0xE92D000E, 0xE3A01C01, h_Logf, (void **)&o_Logf);
    n += ue3_hook("_ZN13FOutputDevice4LogfE5ENamePKwz", 0xE92D000C, 0xE3A020FF, h_LogfType, (void **)&o_LogfType);
    l_info("[UE3] engine log: %d of 2 functions hooked, %s", n, enabled ? "copied to this log" : "off");
}
