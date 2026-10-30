#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <kubridge.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "reimpl/ue3_trace.h"
#include "reimpl/load_stats.h"
#include "reimpl/tex_prefetch.h"
#include "utils/logger.h"

extern so_module so_mod;

/* Install `dst` over the ARM function `sym`, and give back a trampoline that runs the original (thread safe, unlike
 * so_util's SO_CONTINUE). The first two instructions are copied into the trampoline, so they must match w0/w1 (w1 = 0:
 * not checked). Either may be "ldr rX, [pc, #imm]" (the usual literal load): its literal is copied next to the
 * trampoline and the load pointed at it. Anything else that depends on pc refuses the hook. */
static int is_ldr_lit(uint32_t ins) { return (ins & 0x0F7F0000u) == 0x051F0000u && (ins >> 28) != 15 && ((ins >> 12) & 15) != 15; }
static int is_pc_dependent(uint32_t ins) {
    if ((ins & 0xF0000000u) == 0xF0000000u) return 1;            /* unconditional-space instructions */
    if ((ins & 0x0E000000u) == 0x0A000000u) return 1;            /* b / bl */
    if (((ins >> 16) & 15) == 15 || ((ins >> 12) & 15) == 15) return 1;   /* reads or writes pc */
    if ((ins & 0x0E000010u) == 0x00000000u && (ins & 15) == 15) return 1; /* pc as shifted operand */
    return 0;
}
int ue3_hook(const char *sym, uint32_t w0, uint32_t w1, void *dst, void **orig) {
    uintptr_t a = (uintptr_t)so_symbol(&so_mod, sym);
    if (!a) {
        l_warn("hook: %s not found, not hooked.", sym);
        return 0;
    }
    const uint32_t *code = (const uint32_t *)a;
    if ((a & 1) || code[0] != w0 || (w1 && code[1] != w1)) {
        l_warn("hook: %s starts differently than expected (%08x %08x), not hooked.", sym, (unsigned)code[0], (unsigned)code[1]);
        return 0;
    }
    uint32_t tramp[6] = {code[0], code[1], 0xE51FF004 /* ldr pc, [pc, #-4] */, (uint32_t)(a + 8), 0, 0};
    for (int k = 0; k < 2; k++) {
        uint32_t ins = code[k];
        if (is_ldr_lit(ins)) {
            unsigned imm = ins & 0xFFF;
            uintptr_t la = a + 4 * k + 8;
            la = (ins & (1u << 23)) ? la + imm : la - imm;
            tramp[4 + k] = *(const uint32_t *)la;
            /* in the trampoline: instruction k at +4k reads pc = +4k+8; its literal sits at +16+4k -> imm 8 */
            tramp[k] = (ins & 0xFFFFF000u) | (1u << 23) | 8u;
        } else if (is_pc_dependent(ins)) {
            l_warn("hook: %s starts with a pc-relative instruction (%08x), not hooked.", sym, (unsigned)ins);
            return 0;
        }
    }
    uintptr_t head = (so_mod.patch_head + 3) & ~(uintptr_t)3;
    if (head + sizeof(tramp) > so_mod.patch_base + so_mod.patch_size) {
        l_warn("hook: no room for a trampoline for %s.", sym);
        return 0;
    }
    kuKernelCpuUnrestrictedMemcpy((void *)head, tramp, sizeof(tramp));
    kuKernelFlushCaches((void *)head, sizeof(tramp));
    so_mod.patch_head = head + sizeof(tramp);
    *orig = (void *)head;
    hook_addr(a, (uintptr_t)dst);
    return 1;
}

/* UTF-32 (the engine's wchar_t is 4 bytes) -> UTF-8 */
static void w2u(const uint32_t *w, char *out, size_t n) {
    size_t o = 0;
    for (; w && *w && o + 5 < n; w++) {
        uint32_t c = *w;
        if (c < 0x80)         { out[o++] = (char)c; }
        else if (c < 0x800)   { out[o++] = 0xC0 | (c >> 6);  out[o++] = 0x80 | (c & 0x3F); }
        else if (c < 0x10000) { out[o++] = 0xE0 | (c >> 12); out[o++] = 0x80 | ((c >> 6) & 0x3F); out[o++] = 0x80 | (c & 0x3F); }
        else                  { out[o++] = 0xF0 | (c >> 18); out[o++] = 0x80 | ((c >> 12) & 0x3F); out[o++] = 0x80 | ((c >> 6) & 0x3F); out[o++] = 0x80 | (c & 0x3F); }
    }
    out[o] = 0;
}

static void *(*o_unzOpen2)(const char *, void *);
static void *h_unzOpen2(const char *path, void *funcs) {
    void *r = o_unzOpen2(path, funcs);
    l_info("[ue3] unzOpen2(\"%s\") -> %p%s", path ? path : "(null)", r, r ? "" : "   <-- FAILED to open as a zip");
    return r;
}

static int (*o_FileSize)(void *, const uint32_t *);
static int h_FileSize(void *self, const uint32_t *file) {
    int r = o_FileSize(self, file);
    char b[512];
    w2u(file, b, sizeof(b));
    l_info("[ue3] FileSize(\"%s\") -> %d", b, r);
    return r;
}

static void (*o_FindFiles)(void *, void *, const uint32_t *, unsigned, unsigned);
static void h_FindFiles(void *self, void *result, const uint32_t *file, unsigned files, unsigned dirs) {
    o_FindFiles(self, result, file, files, dirs);
    char b[512];
    w2u(file, b, sizeof(b));
    /* TArray<FString>: { Data*, ArrayNum, ArrayMax } */
    l_info("[ue3] FindFiles(\"%s\", files=%u, dirs=%u) -> %d result(s)", b, files, dirs, result ? ((int *)result)[1] : -1);
}

/* ---- text measuring vs drawing (journal paragraphs and save slots overlapping by one line) ----
 * UCanvas::WrappedPrint(Draw, XL, YL, Font, ScaleX, ScaleY, Center, Text, RenderInfo, ...) both measures (Draw = 0)
 * and draws (Draw = 1) wrapped text inside the canvas clip; ClippedStrLen measures without wrapping. Each distinct
 * (text, width, result) is logged once, so a measuring call that wraps differently from the drawing call shows up.
 * The game's own menu labels measure through UCanvas::Horror_TextSize -> ClippedStrLen.
 * Canvas fields as WrappedPrint reads them: OrgX 64, OrgY 68, ClipX 80, ClipY 84, CurX 88, CurY 92. */
static int text_trace = 1;
static uint32_t seen[512];
static int seen_n;

static int seen_once(uint32_t h) {
    for (int i = 0; i < seen_n; i++) if (seen[i] == h) return 0;
    if (seen_n < (int)(sizeof(seen) / sizeof(seen[0]))) seen[seen_n++] = h;
    return seen_n < (int)(sizeof(seen) / sizeof(seen[0]));
}

static uint32_t text_hash(const uint32_t *t, uint32_t h) {
    for (int i = 0; t && t[i] && i < 4096; i++) h = h * 31 + t[i];
    return h;
}

static int text_len(const uint32_t *t) { int n = 0; while (t && t[n] && n < 4096) n++; return n; }

/* ---- the engine's own log ----
 * This is a shipping build: appOutputDebugString is an empty function and nothing the engine logs goes anywhere.
 * FOutputDevice::Logf is hooked to copy each message into log.txt as [UE3] (first 600 messages; repeats of the same
 * text are counted, not repeated) and then hands the formatted text on to the original. Formats are UE3 TCHAR
 * strings: wchar_t is 4 bytes and %s takes a TCHAR string. DM_UE3_LOG 0 in config.txt turns this off. */
static int ue3_log_on = 1;
static void tfmt(char *out, size_t n, const uint32_t *f, va_list ap) {
    size_t o = 0;
#define PUT(c) do { if (o + 1 < n) out[o++] = (char)(c); } while (0)
    for (; f && *f && o + 1 < n; f++) {
        if (*f != '%') { PUT(*f < 0x80 ? *f : '?'); continue; }
        char spec[16]; int sl = 0;
        spec[sl++] = '%';
        f++;
        while (*f && strchr("-+ #0123456789.*", (int)*f) && sl < 10) {
            if (*f == '*') { sl += snprintf(spec + sl, sizeof(spec) - sl, "%d", va_arg(ap, int)); f++; continue; }
            spec[sl++] = (char)*f++;
        }
        int lcount = 0;
        while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'I') { if (*f == 'l') lcount++; f++; }
        uint32_t c = *f;
        if (!c) break;
        char tmp[256];
        tmp[0] = 0;
        switch (c) {
        case '%': PUT('%'); continue;
        case 's': case 'S': {
            const uint32_t *ws = va_arg(ap, const uint32_t *);
            char u[256];
            static const uint32_t nul[] = { '(', 'n', 'u', 'l', 'l', ')', 0 };
            w2u(ws ? ws : nul, u, sizeof(u));
            spec[sl++] = 's'; spec[sl] = 0;
            snprintf(tmp, sizeof(tmp), spec, u);
            break;
        }
        case 'c': { uint32_t ch = (uint32_t)va_arg(ap, int); tmp[0] = ch < 0x80 ? (char)ch : '?'; tmp[1] = 0; break; }
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
            if (lcount >= 2) { spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = (char)c; spec[sl] = 0; snprintf(tmp, sizeof(tmp), spec, va_arg(ap, long long)); }
            else { spec[sl++] = (char)c; spec[sl] = 0; snprintf(tmp, sizeof(tmp), spec, va_arg(ap, int)); }
            break;
        case 'f': case 'F': case 'g': case 'G': case 'e': case 'E':
            spec[sl++] = (char)c; spec[sl] = 0; snprintf(tmp, sizeof(tmp), spec, va_arg(ap, double)); break;
        case 'p': snprintf(tmp, sizeof(tmp), "%p", va_arg(ap, void *)); break;
        default: PUT('%'); PUT(c < 0x80 ? c : '?'); continue;
        }
        for (char *t = tmp; *t; t++) PUT(*t);
    }
    out[o] = 0;
#undef PUT
}

static void ue3_log_line(int type, const char *text) {
    static unsigned n, last_hash, repeats;
    unsigned h = 5381;
    for (const char *t = text; *t; t++) h = h * 33 + (unsigned char)*t;
    if (h == last_hash) { repeats++; return; }
    if (repeats) { if (n < 600) l_info("[UE3] (previous message repeated %u more times)", repeats); repeats = 0; }
    last_hash = h;
    if (n++ >= 600) { if (n == 601) l_info("[UE3] (600 messages logged; the rest are not)"); return; }
    if (type >= 0) l_info("[UE3] (%d) %s", type, text);
    else l_info("[UE3] %s", text);
}

static const uint32_t pct_s[] = { '%', 's', 0 };
static void (*o_Logf)(void *, const uint32_t *, ...);
static void h_Logf(void *self, const uint32_t *fmt, ...) {
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    tfmt(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (ue3_log_on) ue3_log_line(-1, text);
    uint32_t w[1024];
    size_t i = 0;
    for (; text[i] && i < 1023; i++) w[i] = (unsigned char)text[i];
    w[i] = 0;
    o_Logf(self, pct_s, w);
}
static void (*o_LogfN)(void *, int, const uint32_t *, ...);
static void h_LogfN(void *self, int type, const uint32_t *fmt, ...) {
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    tfmt(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (ue3_log_on) ue3_log_line(type, text);
    uint32_t w[1024];
    size_t i = 0;
    for (; text[i] && i < 1023; i++) w[i] = (unsigned char)text[i];
    w[i] = 0;
    o_LogfN(self, type, pct_s, w);
}

/* ---- hitch profiler ----
 * What the game thread is doing during a long frame: synchronous package loads (UObject::LoadPackage), waits for the
 * async loader (FlushAsyncLoading) and garbage collection (CollectGarbage) are timed, and anything over HITCH_MS is
 * logged as [hitch]. Async load requests are listed too (first 200), so a hitch can be matched with what was asked for. */
#define HITCH_MS 30
static unsigned hitch_n;
static uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }
static void hitch(const char *what, const char *detail, uint64_t t0) {
    uint32_t ms = (uint32_t)((now_us() - t0) / 1000);
    if (ms >= HITCH_MS && hitch_n++ < 300) l_info("[hitch] %s%s%s: %u ms", what, detail ? " " : "", detail ? detail : "", ms);
}

/* r57: loads get a breakdown of where their time went (load_stats.h). Loads can nest (a LoadPackage inside a
 * flush): only the outermost is broken down. */
static volatile int load_depth;
static void hitch_load(const char *what, const char *detail, const load_snap_t *a, int outer) {
    uint32_t ms = (uint32_t)((now_us() - a->t_us) / 1000);
    if (ms < HITCH_MS || hitch_n++ >= 300) return;
    char bd[1100];
    bd[0] = 0;
    if (outer) load_breakdown(a, bd, sizeof(bd));
    l_info("[hitch] %s%s%s: %u ms%s", what, detail ? " " : "", detail ? detail : "", ms, bd);
}

static void *(*o_LoadPackage)(void *, const uint32_t *, unsigned);
static void *h_LoadPackage(void *outer, const uint32_t *file, unsigned flags) {
    load_snap_t s;
    load_snap(&s);
    int top = __sync_add_and_fetch(&load_depth, 1) == 1;
    texpf_loading(1);
    void *r = o_LoadPackage(outer, file, flags);
    texpf_loading(-1);
    __sync_sub_and_fetch(&load_depth, 1);
    char b[160];
    w2u(file, b, sizeof(b));
    hitch_load("LoadPackage", b, &s, top);
    texpf_package_loaded(b);
    return r;
}

static void (*o_Flush)(uint32_t, uint32_t);
static void h_Flush(uint32_t a, uint32_t b) {
    load_snap_t s;
    load_snap(&s);
    int top = __sync_add_and_fetch(&load_depth, 1) == 1;
    texpf_loading(1);
    o_Flush(a, b);
    texpf_loading(-1);
    __sync_sub_and_fetch(&load_depth, 1);
    hitch_load("FlushAsyncLoading (waited for the async loader)", NULL, &s, top);
}

static void (*o_GC)(uint64_t, unsigned);
/* r74: a normal garbage collection less than 2 s after the last one is skipped (DM_GC_SKIP_REPEAT, on by
 * default). r71: 36 of 130 ran under a second after the previous one - mostly as a fight ended and during the summary -
 * each 30-69 ms of frozen game for next to nothing (the previous pass had just freed what there was). Skipping only
 * frees the memory later: the next pass collects it. Never two skips in a row, and full purges (level changes, which
 * unload areas) always run. */
int dm_gc_skip_repeat = 1;
static uint64_t gc_last_end;
static int gc_skipped_last;
static void h_GC(uint64_t keep, unsigned purge) {
    uint64_t t0 = now_us();
    if (dm_gc_skip_repeat && !purge && !gc_skipped_last && gc_last_end && t0 - gc_last_end < 2000000ull) {
        gc_skipped_last = 1;
        static unsigned n;
        if (n++ < 200) l_info("[hitch] CollectGarbage skipped: %u ms after the last one (%u skipped so far)",
                              (unsigned)((t0 - gc_last_end) / 1000), n);
        return;
    }
    gc_skipped_last = 0;
    o_GC(keep, purge);
    gc_last_end = now_us();
    hitch("CollectGarbage", purge ? "(full purge)" : NULL, t0);
}

typedef struct { uint32_t *data; int num, max; } fstr_t;
static int (*o_LoadAsync)(const fstr_t *, void *, void *, const void *, uint32_t, uint32_t);
static int h_LoadAsync(const fstr_t *name, void *cb, void *ud, const void *guid, uint32_t t0, uint32_t t1) {
    static int n;
    if (n++ < 200 && name && name->data) {
        char b[160];
        w2u(name->data, b, sizeof(b));
        l_info("[hitch] async load requested: %s", b);
    }
    return o_LoadAsync(name, cb, ud, guid, t0, t1);
}

/* ---- smaller wrapped text (DM_TEXT_SCALE, default 0.8) ----
 * The game places paragraphs (journal, notes) from an estimate: unwrapped width / box width x ~29.1 px (31 px lines x
 * 960/1024), at least 65 px. Wrapping wastes room at every line end and lines are 31 px apart, so the estimate is short
 * and the next paragraph overlaps the last line; long subtitles run off the bottom of the screen. r39/r40 tried to
 * correct the measurement, but the journal computes its layout once when a page opens. Instead, text that has to wrap
 * is drawn (and wrap-measured) at DM_TEXT_SCALE of its size, while the game's estimate still uses the full size, so
 * the space it reserves is enough: at 0.8 a paragraph needs ~(width/box) x 21 px + 12 px against ~29 px reserved.
 * Single-line text (menus, labels, centred messages) is left alone. 1.0 turns it off. */
static float text_scale = 0.8f;
static void (*o_ClippedStrLen)(void *, float, float, int *, int *, const uint32_t *);

static void (*o_WrappedPrint)(void *, unsigned, int *, int *, void *, float, float, unsigned, const uint32_t *, const void *, float, float);

/* WrappedPrint centres each line with its unscaled width, so scaled centred text (subtitles, titles) drifts left by
 * (1/scale - 1) x half the line width (r41 screenshots). For those, wrap here and draw line by line, each one placed
 * at the centre itself. The centring does not come from WrappedPrint's Center argument (DrawWrappedText ignores it)
 * but from the canvas's bCenter flag (bit 0 of the byte at offset 0x6c), so that flag is cleared while the lines are
 * drawn (r42 left it on and every line after the first was centred twice). Canvas CurX/CurY (floats 22/23) and the
 * flag are restored afterwards; WrappedPrint itself does not move CurX/CurY. */
#define CANVAS_CENTER_BYTE 0x6c
static int is_space(uint32_t c) { return c == ' ' || c == '\t'; }

static void draw_centred_scaled(void *self, int *xl, int *yl, void *font, float sx, float sy, const uint32_t *text,
                                const void *ri, float a, float b) {
    float *cf = (float *)self;
    float curx = cf[22], cury = cf[23], w = cf[20] - cf[16] - curx;
    uint8_t *cflag = (uint8_t *)self + CANVAS_CENTER_BYTE, saved_flag = *cflag;
    int n = text_len(text);
    uint32_t *line = malloc(((size_t)n + 1) * sizeof(uint32_t));
    if (!line) { o_WrappedPrint(self, 1, xl, yl, font, sx, sy, 1, text, ri, a, b); return; }
    *cflag = (uint8_t)(saved_flag & ~1u);
    int pos = 0, row = 0, maxw = 0, lh = 0;
    while (pos < n) {
        while (pos < n && is_space(text[pos])) pos++;
        if (pos >= n) break;
        if (text[pos] == '\n') { pos++; row++; continue; }
        /* grow the line word by word while it fits; stop at a newline */
        int end = pos, best = -1, best_w = 0, h = 0;
        while (end < n && text[end] != '\n') {
            int we = end;
            while (we < n && !is_space(text[we]) && text[we] != '\n') we++;
            int len = we - pos, lw = 0, lhh = 0;
            memcpy(line, text + pos, (size_t)len * sizeof(uint32_t));
            line[len] = 0;
            o_ClippedStrLen(font, sx, sy, &lw, &lhh, line);
            if ((float)lw > w && best >= 0) break;   /* the word does not fit: break before it */
            best = we; best_w = lw; h = lhh;
            end = we;
            while (end < n && is_space(text[end])) end++;
            if ((float)lw > w) break;                /* a single word wider than the box: keep it alone */
        }
        if (best < 0) best = end;
        int len = best - pos;
        memcpy(line, text + pos, (size_t)len * sizeof(uint32_t));
        line[len] = 0;
        if (len > 0) {
            if (!lh) lh = h;
            cf[22] = curx + (w - (float)best_w) * 0.5f;
            cf[23] = cury + (float)(row * lh);
            int dx = 0, dy = 0;
            o_WrappedPrint(self, 1, &dx, &dy, font, sx, sy, 0, line, ri, a, b);
            if (best_w > maxw) maxw = best_w;
            row++;
        }
        pos = best;
        if (pos < n && text[pos] == '\n') pos++;
    }
    cf[22] = curx;
    cf[23] = cury;
    *cflag = saved_flag;
    free(line);
    if (xl) *xl = maxw;
    if (yl) *yl = row * lh;
}

static void h_WrappedPrint(void *self, unsigned draw, int *xl, int *yl, void *font, float sx, float sy, unsigned center,
                           const uint32_t *text, const void *ri, float a, float b) {
    const float *cf = (const float *)self;
    float orgx = cf[16], orgy = cf[17], clipx = cf[20], clipy = cf[21], curx = cf[22], cury = cf[23];
    int scaled = 0;
    int centred = center || (((const uint8_t *)self)[CANVAS_CENTER_BYTE] & 1);
    /* r64: subtitles (centred, in the band at y ~435 that starts 48 px in) are always drawn at DM_TEXT_SCALE, so a
     * short line is not bigger than a long one that had to wrap */
    /* r70: exactly the subtitle box (x 48-912, y 435.2): r64's band (y 400-480, wide, centred) also caught the final
     * credits as they scrolled through it, so their lines changed size on the way up */
    int subtitle = centred && fabsf(orgy + cury - 435.2f) < 1.5f && fabsf(curx - 48.f) < 1.f && fabsf(clipx - 912.f) < 1.f && orgx == 0.f;
    if (text_scale < 0.999f && o_ClippedStrLen && font && (subtitle || text_len(text) >= 24)) {
        int uw = 0, uh = 0;
        if (!subtitle) o_ClippedStrLen(font, sx, sy, &uw, &uh, text);   /* unwrapped width at the requested size */
        if (subtitle || (float)uw > clipx - orgx - curx) { sx *= text_scale; sy *= text_scale; scaled = 1; }
    }
    if (scaled && draw && centred) draw_centred_scaled(self, xl, yl, font, sx, sy, text, ri, a, b);
    else o_WrappedPrint(self, draw, xl, yl, font, sx, sy, center, text, ri, a, b);
    if (!text_trace || text_len(text) < 6) return;
    uint32_t h = text_hash(text, draw * 7919u + (uint32_t)(clipx * 8) + (uint32_t)(curx * 16) + (uint32_t)(sx * 1000));
    h = h * 31 + (uint32_t)(xl ? *xl : 0) * 131 + (uint32_t)(yl ? *yl : 0);
    if (!seen_once(h)) return;
    char t[72];
    w2u(text, t, sizeof(t));
    l_info("[text] %s%s scale %.3f/%.3f width %.1f (clip %.1f - org %.1f - cur %.1f) at y %.1f%+.1f -> %d x %d  \"%s%s\"",
           draw ? "DRAW   " : "MEASURE", (center || (((const uint8_t *)self)[CANVAS_CENTER_BYTE] & 1)) ? " centred" : "", (double)sx, (double)sy, (double)(clipx - orgx - curx), (double)clipx, (double)orgx,
           (double)curx, (double)orgy, (double)cury, xl ? *xl : -1, yl ? *yl : -1, t, text_len(text) > 60 ? "..." : "");
    (void)clipy;
}

/* static UCanvas::ClippedStrLen(UFont*, ScaleX, ScaleY, int& XL, int& YL, const TCHAR*): no "this" (r37 assumed one
 * and crashed when the game's Horror_TextSize called it) */
static void h_ClippedStrLen(void *font, float sx, float sy, int *xl, int *yl, const uint32_t *text) {
    o_ClippedStrLen(font, sx, sy, xl, yl, text);
    if (!text_trace || text_len(text) < 24) return;
    uint32_t h = text_hash(text, 0x51ED + (uint32_t)(sx * 1000)) * 31 + (uint32_t)(xl ? *xl : 0) * 131 + (uint32_t)(yl ? *yl : 0);
    if (!seen_once(h)) return;
    char t[72];
    w2u(text, t, sizeof(t));
    l_info("[text] SIZE    scale %.3f/%.3f unwrapped -> %d x %d  \"%s%s\"", (double)sx, (double)sy, xl ? *xl : -1,
           yl ? *yl : -1, t, text_len(text) > 60 ? "..." : "");
}


/* ---- r59: which game screen is up (for the physical-control profiles, padinput.c) ----
 * The game's menus and HUD are UHorrorMenuScene objects drawn every frame by RenderScene_Horror (the base class and
 * four overrides). Each scene drawn is classified by its class name: the in-game HUD vs. anything else (pause menu,
 * store, journal, pickups, combat summary...). Each new scene object is logged once as [ui]. GotoState of any object is
 * logged once per (class, state), to find a reliable "in a fight" signal. */
typedef struct { uint32_t *data; int num, max; } tstr_t;
static void (*e_GetFullName)(tstr_t *out, const void *self, const void *stop);
static void (*e_FNameToString)(tstr_t *out, const void *fname);
static void (*e_appFree)(void *);
static void tstr_free(tstr_t *t) { if (t->data && e_appFree) e_appFree(t->data); t->data = NULL; }
volatile uint64_t ui_hud_us, ui_menu_us;      /* last time a HUD / a non-HUD scene was drawn */
volatile uint64_t ui_fighthud_us;             /* r62: last time the fight screen (HorrorMenuHUD) was drawn */
volatile uint64_t ui_fight_end_us;            /* r64: a fight ended (the player controller went to PostCombatTransition) */
volatile int ui_post_combat;                  /* r64: the player controller is still in PostCombatTransition */
volatile uint64_t ui_post_exit_us;            /* r65: when it left PostCombatTransition (the summary was dismissed) */
volatile char ui_menu_name[64];
#define UI_SEEN 128
static struct { const void *obj; int hud; char cls[48]; } ui_seen[UI_SEEN];
static int ui_seen_n;
static pthread_mutex_t ui_lock = PTHREAD_MUTEX_INITIALIZER;

static void tstr_to_utf8(const tstr_t *t, char *out, size_t n) {
    if (t->data && t->num > 0) w2u(t->data, out, n);
    else snprintf(out, n, "?");
}

/* 1 = HUD-like (gameplay), 0 = a menu. DM_PAD_GAMEPLAY_SCENES in config.txt adds class-name parts. */
static char gameplay_parts[256] = "HUD PauseButton PopupText WitchText Notification Subtitle";   /* r60: + PauseButton (the exploration screen), PopupText, WitchText */
static int is_gameplay_scene(const char *cls) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", gameplay_parts);
    for (char *t = strtok(tmp, " ,"); t; t = strtok(NULL, " ,")) if (strstr(cls, t)) return 1;
    return 0;
}

static void ui_scene(const void *scene) {
    int hud = -1;
    char cls[48] = "";
    pthread_mutex_lock(&ui_lock);
    for (int i = 0; i < ui_seen_n; i++) if (ui_seen[i].obj == scene) { hud = ui_seen[i].hud; snprintf(cls, sizeof(cls), "%s", ui_seen[i].cls); break; }
    pthread_mutex_unlock(&ui_lock);
    if (hud < 0) {
        char full[160] = "?";
        if (e_GetFullName) {
            tstr_t t = {0};
            e_GetFullName(&t, scene, NULL);   /* "Class Outer.Name" */
            tstr_to_utf8(&t, full, sizeof(full));
            tstr_free(&t);
        }
        char *sp = strchr(full, ' ');
        snprintf(cls, sizeof(cls), "%.*s", sp ? (int)(sp - full) : (int)strlen(full), full);
        hud = is_gameplay_scene(cls);
        pthread_mutex_lock(&ui_lock);
        static int ring;
        int slot = ui_seen_n < UI_SEEN ? ui_seen_n++ : (ring++ % UI_SEEN);   /* full: replace the oldest */
        ui_seen[slot].obj = scene;
        ui_seen[slot].hud = hud;
        snprintf(ui_seen[slot].cls, 48, "%s", cls);
        pthread_mutex_unlock(&ui_lock);
        static int nlog;
        if (nlog++ < 150) l_info("[ui] scene %s (%s)", full, hud ? "gameplay" : "menu");
    }
    uint64_t now = sceKernelGetProcessTimeWide();
    if (hud && strstr(cls, "MenuHUD")) ui_fighthud_us = now;
    if (hud) ui_hud_us = now;
    else {
        ui_menu_us = now;
        for (int i = 0; i < 63; i++) { ui_menu_name[i] = cls[i]; if (!cls[i]) break; }
    }
}

typedef void (*render_fn)(void *, void *, float);
static render_fn o_rs[5];
static void h_rs0(void *s, void *c, float d) { ui_scene(s); o_rs[0](s, c, d); }
static void h_rs1(void *s, void *c, float d) { ui_scene(s); o_rs[1](s, c, d); }
static void h_rs2(void *s, void *c, float d) { ui_scene(s); o_rs[2](s, c, d); }
static void h_rs3(void *s, void *c, float d) { ui_scene(s); o_rs[3](s, c, d); }
static void h_rs4(void *s, void *c, float d) { ui_scene(s); o_rs[4](s, c, d); }

static int (*o_GotoState)(void *, uint32_t, uint32_t, uint32_t, uint32_t);

/* r60: the fight state, from the player's combat pawn (HorrorPlayerCombatPawn): Melee... / Block... = sword,
 * Ranged... = crossbow, anything else (NoWeapon, Death, ...) = no fight. Read by padinput.c. */
volatile int ui_combat_mode;        /* 0 none, 1 sword, 2 crossbow */
volatile int ui_combat_seen;        /* the pawn's state has been seen at least once (the hook works) */

/* GotoState runs on the game thread only, so these caches need no lock */
static struct { const void *obj; char cls[40]; } go_obj[64];
static int go_obj_n, go_obj_ring;
static struct { uint32_t idx, num; char name[40]; } go_nm[256];
static int go_nm_n, go_nm_ring;

static const char *go_class(const void *o) {
    for (int i = 0; i < go_obj_n; i++) if (go_obj[i].obj == o) return go_obj[i].cls;
    char full[96] = "?";
    if (e_GetFullName) {
        tstr_t a = {0};
        e_GetFullName(&a, o, NULL);
        tstr_to_utf8(&a, full, sizeof(full));
        tstr_free(&a);
    }
    char *sp = strchr(full, ' ');
    if (sp) *sp = 0;
    int k = go_obj_n < 64 ? go_obj_n++ : (go_obj_ring++ % 64);
    go_obj[k].obj = o;
    snprintf(go_obj[k].cls, sizeof(go_obj[k].cls), "%s", full);
    return go_obj[k].cls;
}

static const char *go_state(uint32_t idx, uint32_t num) {
    for (int i = 0; i < go_nm_n; i++) if (go_nm[i].idx == idx && go_nm[i].num == num) return go_nm[i].name;
    char st[48] = "?";
    if (e_FNameToString) {
        uint32_t fn[2] = { idx, num };
        tstr_t b = {0};
        e_FNameToString(&b, fn);
        tstr_to_utf8(&b, st, sizeof(st));
        tstr_free(&b);
    }
    int k = go_nm_n < 256 ? go_nm_n++ : (go_nm_ring++ % 256);
    go_nm[k].idx = idx;
    go_nm[k].num = num;
    snprintf(go_nm[k].name, sizeof(go_nm[k].name), "%s", st);
    return go_nm[k].name;
}

static int h_GotoState(void *self, uint32_t n_index, uint32_t n_number, uint32_t label, uint32_t force) {
    if (self && e_GetFullName && e_FNameToString) {
        const char *cls = go_class(self), *st = go_state(n_index, n_number);
        if (!strcmp(cls, "HorrorPlayerCombatPawn")) {
            int m = (!strncmp(st, "Melee", 5) || !strncmp(st, "Block", 5)) ? 1 : !strncmp(st, "Ranged", 6) ? 2 : 0;
            if (m != ui_combat_mode) l_info("[ui] fight: %s (player %s)", m == 1 ? "sword" : m == 2 ? "crossbow" : "none", st);
            ui_combat_mode = m;
            ui_combat_seen = 1;
        } else if ((!strcmp(cls, "HorrorWeapon_Sword") || !strcmp(cls, "HorrorWeapon_Crossbow")) &&
                   (!strcmp(st, "WeaponEquipping") || !strcmp(st, "Active")) && !ui_combat_mode) {
            /* r62: the weapon being drawn is the earliest sign of a fight (before the pawn's Melee/Ranged states) */
            int m = cls[13] == 'S' ? 1 : 2;
            l_info("[ui] fight: %s (%s %s)", m == 1 ? "sword" : "crossbow", cls, st);
            ui_combat_mode = m;
            ui_combat_seen = 1;
        }
        if (!strcmp(cls, "HorrorPC")) {   /* r71: every player controller state change, with its time */
            static char last_pc[48];
            if (strcmp(last_pc, st)) {
                static int npc;
                if (npc++ < 400) l_info("[ui] player controller: %s -> %s", last_pc[0] ? last_pc : "-", st);
                snprintf(last_pc, sizeof(last_pc), "%s", st);
            }
        }
        if (!strcmp(cls, "HorrorPC")) {   /* r64: the camera swings back after a fight: no look until this state is left */
            int pc = !strcmp(st, "PostCombatTransition");
            if (pc != ui_post_combat) l_info("[ui] player controller %s post-fight transition (%s)", pc ? "in" : "out of", st);
            if (!pc && ui_post_combat) ui_post_exit_us = sceKernelGetProcessTimeWide();
            ui_post_combat = pc;
            if (pc) ui_fight_end_us = sceKernelGetProcessTimeWide();
        }
        if (!strcmp(cls, "HorrorPC") && (!strcmp(st, "PostCombatTransition") || !strcmp(st, "Dead")) && ui_combat_mode) {
            l_info("[ui] fight: none (player controller %s)", st);   /* the fight is over even if the pawn keeps its state */
            ui_combat_mode = 0;
            ui_fight_end_us = sceKernelGetProcessTimeWide();
        }
        /* each (class, state) logged once, for finding signals */
        static uint32_t seen[512];
        static int ns;
        if (ns < 512) {
            uint32_t h = 5381;
            for (const char *p = cls; *p; p++) h = h * 33 + (unsigned char)*p;
            for (const char *p = st; *p; p++) h = h * 33 + (unsigned char)*p;
            int dup = 0;
            for (int i = 0; i < ns; i++) if (seen[i] == h) { dup = 1; break; }
            if (!dup) {
                seen[ns++] = h;
                l_info("[ui] %s goes to state %s", cls, st);
            }
        }
    }
    return o_GotoState(self, n_index, n_number, label, force);
}

/* r71: the camera's final rotation, every frame (ULocalPlayer::CalcSceneView fills it in). padinput.c pauses the
 * right stick look while the camera turns without a finger dragging (the game moving it: after fights, menus, text
 * boxes, while walking to a tapped spot). Rotation units: 65536 = a full turn. */
volatile uint64_t ui_cam_moved_us;            /* last frame the camera turned more than a hair */
static int32_t cam_last[2];
static int cam_have;
static void *(*o_CalcSceneView)(void *, void *, float *, int32_t *, void *, void *);
static void *h_CalcSceneView(void *self, void *family, float *loc, int32_t *rot, void *viewport, void *drawer) {
    void *r = o_CalcSceneView(self, family, loc, rot, viewport, drawer);
    if (rot) {
        int dp = (int16_t)(uint16_t)(rot[0] - cam_last[0]), dy = (int16_t)(uint16_t)(rot[1] - cam_last[1]);
        if (cam_have && (dp > 24 || dp < -24 || dy > 24 || dy < -24)) ui_cam_moved_us = sceKernelGetProcessTimeWide();
        cam_last[0] = rot[0];
        cam_last[1] = rot[1];
        cam_have = 1;
    }
    return r;
}

static void ui_watch_install(void) {
    e_GetFullName = (void *)so_symbol(&so_mod, "_ZNK7UObject11GetFullNameEPKS_");
    e_FNameToString = (void *)so_symbol(&so_mod, "_ZNK5FName8ToStringEv");
    e_appFree = (void *)so_symbol(&so_mod, "_Z7appFreePv");
    FILE *cf = fopen(DATA_PATH "config.txt", "r");
    if (cf) {
        char line[300];
        while (fgets(line, sizeof(line), cf))
            if (!strncmp(line, "DM_PAD_GAMEPLAY_SCENES", 22)) {
                char *v = line + 22;
                while (*v == ' ' || *v == '\t') v++;
                v[strcspn(v, "\r\n")] = 0;
                strncat(gameplay_parts, " ", sizeof(gameplay_parts) - strlen(gameplay_parts) - 1);
                strncat(gameplay_parts, v, sizeof(gameplay_parts) - strlen(gameplay_parts) - 1);
            }
        fclose(cf);
    }
    int n = 0;
    n += ue3_hook("_ZN16UHorrorMenuScene18RenderScene_HorrorEP7UCanvasf", 0xE92D4070, 0xE59030BC, h_rs0, (void **)&o_rs[0]);
    n += ue3_hook("_ZN17UHorrorMenuPickup18RenderScene_HorrorEP7UCanvasf", 0xE92D4010, 0xED904A1B, h_rs1, (void **)&o_rs[1]);
    n += ue3_hook("_ZN23UHorrorMenuLibraryEntry18RenderScene_HorrorEP7UCanvasf", 0xE92D4010, 0xED904A1B, h_rs2, (void **)&o_rs[2]);
    n += ue3_hook("_ZN24UHorrorMenuCombatSummary18RenderScene_HorrorEP7UCanvasf", 0xE59F3050, 0xE92D4070, h_rs3, (void **)&o_rs[3]);
    n += ue3_hook("_ZN14UHorrorMenuHUD18RenderScene_HorrorEP7UCanvasf", 0xE92D4FF0, 0xE59F5200, h_rs4, (void **)&o_rs[4]);
    int g = ue3_hook("_ZN7UObject9GotoStateE5FNamejj", 0xE92D4FF0, 0xE1A08003, h_GotoState, (void **)&o_GotoState);
    int c = ue3_hook("_ZN12ULocalPlayer13CalcSceneViewEP16FSceneViewFamilyR7FVectorR8FRotatorP9FViewportP18FViewElementDrawer",
                     0xE92D4FF0, 0xED2D8B10, h_CalcSceneView, (void **)&o_CalcSceneView);   /* r71 */
    l_info("[ui] camera watch %s", c ? "on" : "not hooked");
    l_info("[ui] screen watch: %d of 5 scene functions hooked, state changes %s; gameplay scenes: %s", n,
           g ? "logged" : "not hooked", gameplay_parts);
}

void ue3_trace_install(void) {
    int n = 0;
    n += ue3_hook("unzOpen2", 0xE92D4FF0, 0xE251E000, h_unzOpen2, (void **)&o_unzOpen2);
    n += ue3_hook("_ZN19FFileManagerAndroid8FileSizeEPKw", 0xE92D4FF0, 0xE1A05000, h_FileSize, (void **)&o_FileSize);
    n += ue3_hook("_ZN19FFileManagerAndroid9FindFilesER6TArrayI7FString17FDefaultAllocatorEPKwjj", 0xE92D4FF0,
                          0xE1A05000, h_FindFiles, (void **)&o_FindFiles);
    l_info("Tracing %d of 3 engine file functions.", n);
    int k = 0;
    k += ue3_hook("_ZN7UObject11LoadPackageEP8UPackagePKwm", 0xE92D4FF0, 0, h_LoadPackage, (void **)&o_LoadPackage);
    k += ue3_hook("_ZN7UObject17FlushAsyncLoadingE5FName", 0xE59F308C, 0xE92D4030, h_Flush, (void **)&o_Flush);
    k += ue3_hook("_ZN7UObject14CollectGarbageEyj", 0xE92D4FF0, 0, h_GC, (void **)&o_GC);
    k += ue3_hook("_ZN7UObject16LoadPackageAsyncERK7FStringPFvPS_PvES4_PK5FGuid5FName", 0xE92D4FF0, 0xE24DD034,
                  h_LoadAsync, (void **)&o_LoadAsync);
    {
        FILE *cf = fopen(DATA_PATH "config.txt", "r");
        if (cf) {
            char line[128], key[64];
            int v;
            while (fgets(line, sizeof(line), cf))
                if (sscanf(line, "%63s %d", key, &v) == 2 && !strcmp(key, "DM_UE3_LOG")) ue3_log_on = v != 0;
            fclose(cf);
        }
        int lg = 0;
        lg += ue3_hook("_ZN13FOutputDevice4LogfEPKwz", 0xE92D000E, 0xE3A01C01, h_Logf, (void **)&o_Logf);
        lg += ue3_hook("_ZN13FOutputDevice4LogfE5ENamePKwz", 0xE92D000C, 0xE3A020FF, h_LogfN, (void **)&o_LogfN);
        l_info("[UE3] engine log: %d of 2 functions hooked, %s (DM_UE3_LOG)", lg, ue3_log_on ? "copied to this log" : "off");
    }
    l_info("[hitch] profiler: %d of 4 engine functions timed (loads, async-load waits, garbage collection > %d ms)", k, HITCH_MS);
    int m = 0;
    m += ue3_hook("_ZN7UCanvas12WrappedPrintEjRiS0_P5UFontffjPKwRK15FFontRenderInfoff", 0xE92D4FF0, 0xED2D8B02,
                          h_WrappedPrint, (void **)&o_WrappedPrint);
    m += ue3_hook("_ZN7UCanvas13ClippedStrLenEP5UFontffRiS2_PKw", 0xE92D40F0, 0xE24DD02C, h_ClippedStrLen,
                          (void **)&o_ClippedStrLen);
    FILE *cf = fopen(DATA_PATH "config.txt", "r");
    if (cf) {
        char line[128], k[64];
        int v;
        float fv;
        while (fgets(line, sizeof(line), cf)) {
            if (sscanf(line, "%63s %f", k, &fv) == 2 && !strcmp(k, "DM_TEXT_SCALE") && fv > 0.3f && fv <= 1.0f) text_scale = fv;
            if (sscanf(line, "%63s %d", k, &v) == 2) {
                if (!strcmp(k, "DM_TEXT_TRACE")) text_trace = v != 0;
            }
        }
        fclose(cf);
    }
    ui_watch_install();
    l_info("[text] hooked %d of 2 text functions; wrapped text drawn at %.2f (DM_TEXT_SCALE), trace %s (DM_TEXT_TRACE)", m,
           (double)text_scale, text_trace ? "on" : "off");
}
