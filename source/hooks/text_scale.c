/*
 * Smaller wrapped text, so paragraphs and subtitles fit the space the game gives them.
 *
 * The game places paragraphs (journal, notes) from an estimate: unwrapped width / box width x ~29 px, at least 65 px.
 * Wrapping wastes room at every line end and lines are 31 px apart, so the estimate falls short and the next paragraph
 * overlaps the last line; long subtitles ran off the bottom of the screen. The journal computes its layout once when a
 * page opens, so fixing the measurement does not help. Instead, text that has to wrap is drawn (and measured for
 * wrapping) at TEXT_SCALE of its size while the game's estimate still uses the full size, so the space it reserves is
 * enough. Single-line text (menus, labels, centred messages) is left alone. Subtitles are always drawn at TEXT_SCALE,
 * so a short subtitle is not bigger than a long one that had to wrap.
 *
 * UCanvas::WrappedPrint(Draw, XL, YL, Font, ScaleX, ScaleY, Center, Text, RenderInfo, ...) both measures (Draw = 0)
 * and draws (Draw = 1) wrapped text inside the canvas clip; ClippedStrLen measures without wrapping.
 *
 * The first time each text is drawn or measured it is logged as [text] (DM_TEXT_TRACE 0 in config.txt turns this off):
 * with log timestamps it shows when each subtitle appears.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "hooks/hook.h"
#include "hooks/hooks_internal.h"
#include "util/logger.h"

#define TEXT_SCALE 0.8f
#define MIN_WRAP_CHARS 24   /* shorter texts are never scaled (except subtitles) */

/* UCanvas fields, as floats: OrgX 16, OrgY 17, ClipX 20, ClipY 21, CurX 22, CurY 23; bCenter is bit 0 of byte 0x6c */
#define CANVAS_ORGX 16
#define CANVAS_ORGY 17
#define CANVAS_CLIPX 20
#define CANVAS_CURX 22
#define CANVAS_CURY 23
#define CANVAS_CENTER_BYTE 0x6c

/* the subtitle box: x 48-912, line at y 435.2 */
#define SUBTITLE_Y 435.2f
#define SUBTITLE_X 48.0f
#define SUBTITLE_CLIPX 912.0f

static int trace = 1;

static void (*o_ClippedStrLen)(void *, float, float, int *, int *, const uint32_t *);
static void (*o_WrappedPrint)(void *, unsigned, int *, int *, void *, float, float, unsigned, const uint32_t *,
                              const void *, float, float);

static int text_len(const uint32_t *t) {
    int n = 0;
    while (t && t[n] && n < 4096) n++;
    return n;
}

static int is_space(uint32_t c) { return c == ' ' || c == '\t'; }

/* ---- log each text once ---- */
static uint32_t seen[512];
static int seen_n;

static int first_time(uint32_t h) {
    for (int i = 0; i < seen_n; i++)
        if (seen[i] == h) return 0;
    if (seen_n < (int)(sizeof(seen) / sizeof(seen[0]))) seen[seen_n++] = h;
    return seen_n < (int)(sizeof(seen) / sizeof(seen[0]));
}

static uint32_t text_hash(const uint32_t *t, uint32_t h) {
    for (int i = 0; t && t[i] && i < 4096; i++) h = h * 31 + t[i];
    return h;
}

/* WrappedPrint centres each line by its unscaled width, so scaled centred text (subtitles, titles) would drift left.
 * Such text is wrapped here and drawn line by line, each placed at the centre. The centring comes from the canvas's
 * bCenter flag (WrappedPrint's own Center argument is ignored), so the flag is cleared while the lines are drawn and
 * restored afterwards, with CurX/CurY. */
static void draw_centred_scaled(void *canvas, int *xl, int *yl, void *font, float sx, float sy, const uint32_t *text,
                                const void *render_info, float a, float b) {
    float *cf = (float *)canvas;
    float curx = cf[CANVAS_CURX], cury = cf[CANVAS_CURY], width = cf[CANVAS_CLIPX] - cf[CANVAS_ORGX] - curx;
    uint8_t *center_flag = (uint8_t *)canvas + CANVAS_CENTER_BYTE, saved_flag = *center_flag;
    int n = text_len(text);
    uint32_t *line = malloc(((size_t)n + 1) * sizeof(uint32_t));
    if (!line) {
        o_WrappedPrint(canvas, 1, xl, yl, font, sx, sy, 1, text, render_info, a, b);
        return;
    }
    *center_flag = (uint8_t)(saved_flag & ~1u);

    int pos = 0, row = 0, max_w = 0, line_h = 0;
    while (pos < n) {
        while (pos < n && is_space(text[pos])) pos++;
        if (pos >= n) break;
        if (text[pos] == '\n') {
            pos++;
            row++;
            continue;
        }
        /* grow the line word by word while it fits; stop at a newline */
        int end = pos, best = -1, best_w = 0, h = 0;
        while (end < n && text[end] != '\n') {
            int word_end = end;
            while (word_end < n && !is_space(text[word_end]) && text[word_end] != '\n') word_end++;
            int len = word_end - pos, lw = 0, lh = 0;
            memcpy(line, text + pos, (size_t)len * sizeof(uint32_t));
            line[len] = 0;
            o_ClippedStrLen(font, sx, sy, &lw, &lh, line);
            if ((float)lw > width && best >= 0) break;   /* the word does not fit: break before it */
            best = word_end;
            best_w = lw;
            h = lh;
            end = word_end;
            while (end < n && is_space(text[end])) end++;
            if ((float)lw > width) break;                /* a single word wider than the box stays alone */
        }
        if (best < 0) best = end;
        int len = best - pos;
        memcpy(line, text + pos, (size_t)len * sizeof(uint32_t));
        line[len] = 0;
        if (len > 0) {
            if (!line_h) line_h = h;
            cf[CANVAS_CURX] = curx + (width - (float)best_w) * 0.5f;
            cf[CANVAS_CURY] = cury + (float)(row * line_h);
            int dx = 0, dy = 0;
            o_WrappedPrint(canvas, 1, &dx, &dy, font, sx, sy, 0, line, render_info, a, b);
            if (best_w > max_w) max_w = best_w;
            row++;
        }
        pos = best;
        if (pos < n && text[pos] == '\n') pos++;
    }

    cf[CANVAS_CURX] = curx;
    cf[CANVAS_CURY] = cury;
    *center_flag = saved_flag;
    free(line);
    if (xl) *xl = max_w;
    if (yl) *yl = row * line_h;
}

static void h_WrappedPrint(void *canvas, unsigned draw, int *xl, int *yl, void *font, float sx, float sy,
                           unsigned center, const uint32_t *text, const void *render_info, float a, float b) {
    const float *cf = (const float *)canvas;
    float orgx = cf[CANVAS_ORGX], orgy = cf[CANVAS_ORGY], clipx = cf[CANVAS_CLIPX];
    float curx = cf[CANVAS_CURX], cury = cf[CANVAS_CURY];
    int centred = center || (((const uint8_t *)canvas)[CANVAS_CENTER_BYTE] & 1);
    int subtitle = centred && orgx == 0.f && fabsf(orgy + cury - SUBTITLE_Y) < 1.5f && fabsf(curx - SUBTITLE_X) < 1.f &&
                   fabsf(clipx - SUBTITLE_CLIPX) < 1.f;

    int scaled = 0;
    if (o_ClippedStrLen && font && (subtitle || text_len(text) >= MIN_WRAP_CHARS)) {
        int unwrapped_w = 0, unwrapped_h = 0;
        if (!subtitle) o_ClippedStrLen(font, sx, sy, &unwrapped_w, &unwrapped_h, text);
        if (subtitle || (float)unwrapped_w > clipx - orgx - curx) {
            sx *= TEXT_SCALE;
            sy *= TEXT_SCALE;
            scaled = 1;
        }
    }
    if (scaled && draw && centred) draw_centred_scaled(canvas, xl, yl, font, sx, sy, text, render_info, a, b);
    else o_WrappedPrint(canvas, draw, xl, yl, font, sx, sy, center, text, render_info, a, b);

    if (!trace || text_len(text) < 6) return;
    uint32_t h = text_hash(text, draw * 7919u + (uint32_t)(clipx * 8) + (uint32_t)(curx * 16) + (uint32_t)(sx * 1000));
    h = h * 31 + (uint32_t)(xl ? *xl : 0) * 131 + (uint32_t)(yl ? *yl : 0);
    if (!first_time(h)) return;
    char t[72];
    ue3_wstr_to_utf8(text, t, sizeof(t));
    l_info("[text] %s%s scale %.3f/%.3f width %.1f (clip %.1f - org %.1f - cur %.1f) at y %.1f%+.1f -> %d x %d  \"%s%s\"",
           draw ? "DRAW   " : "MEASURE", centred ? " centred" : "", (double)sx, (double)sy, (double)(clipx - orgx - curx),
           (double)clipx, (double)orgx, (double)curx, (double)orgy, (double)cury, xl ? *xl : -1, yl ? *yl : -1, t,
           text_len(text) > 60 ? "..." : "");
}

/* UCanvas::ClippedStrLen is static: no canvas argument */
static void h_ClippedStrLen(void *font, float sx, float sy, int *xl, int *yl, const uint32_t *text) {
    o_ClippedStrLen(font, sx, sy, xl, yl, text);
    if (!trace || text_len(text) < MIN_WRAP_CHARS) return;
    uint32_t h = text_hash(text, 0x51ED + (uint32_t)(sx * 1000)) * 31 + (uint32_t)(xl ? *xl : 0) * 131 + (uint32_t)(yl ? *yl : 0);
    if (!first_time(h)) return;
    char t[72];
    ue3_wstr_to_utf8(text, t, sizeof(t));
    l_info("[text] SIZE    scale %.3f/%.3f unwrapped -> %d x %d  \"%s%s\"", (double)sx, (double)sy, xl ? *xl : -1,
           yl ? *yl : -1, t, text_len(text) > 60 ? "..." : "");
}

void text_scale_install(void) {
    trace = config_bool("DM_TEXT_TRACE", 1);
    int n = 0;
    n += ue3_hook("_ZN7UCanvas12WrappedPrintEjRiS0_P5UFontffjPKwRK15FFontRenderInfoff", 0xE92D4FF0, 0xED2D8B02,
                  h_WrappedPrint, (void **)&o_WrappedPrint);
    n += ue3_hook("_ZN7UCanvas13ClippedStrLenEP5UFontffRiS2_PKw", 0xE92D40F0, 0xE24DD02C, h_ClippedStrLen,
                  (void **)&o_ClippedStrLen);
    l_info("[text] hooked %d of 2 text functions; wrapped text drawn at %.2f, trace %s", n, (double)TEXT_SCALE,
           trace ? "on" : "off");
}
