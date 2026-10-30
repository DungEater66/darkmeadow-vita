/*
 * PVRTC 1 (2 and 4 bits per pixel, with or without alpha) decoder, to RGBA8888.
 *
 * Only used by the DM_TEST_PVRTC_SOFTWARE switch (utils/gldiag.c): it uploads decoded pixels instead of the compressed
 * data, to tell whether a texture problem is in how the console's GPU samples PVRTC. It follows the structure of
 * Imagination's reference decompressor (PVRTDecompress): each 4x4 (4 bpp) or 8x4 (2 bpp) block holds two colours
 * and per-pixel modulation weights; the colours are bilinearly upscaled across neighbouring blocks (wrapping at the
 * edges) and blended by the weights. Blocks are stored in Morton ("twiddled") order. Checked bit for bit against an
 * independent decoder (texture2ddecoder) on random data.
 */
#include "utils/pvrtc.h"

#include <stdlib.h>
#include <string.h>

typedef struct { int r, g, b, a; } px_t;
typedef struct { uint32_t mod, col; } word_t;

static px_t colour_a(uint32_t c) {
    px_t p;
    if (c & 0x8000) {
        p.r = (c & 0x7c00) >> 10;
        p.g = (c & 0x3e0) >> 5;
        p.b = (c & 0x1e) | ((c & 0x1e) >> 4);
        p.a = 0xf;
    } else {
        p.r = ((c & 0xf00) >> 7) | ((c & 0xf00) >> 11);
        p.g = ((c & 0xf0) >> 3) | ((c & 0xf0) >> 7);
        p.b = ((c & 0xe) << 1) | ((c & 0xe) >> 2);
        p.a = (c & 0x7000) >> 11;
    }
    return p;
}

static px_t colour_b(uint32_t c) {
    px_t p;
    if (c & 0x80000000u) {
        p.r = (c & 0x7c000000) >> 26;
        p.g = (c & 0x3e00000) >> 21;
        p.b = (c & 0x1f0000) >> 16;
        p.a = 0xf;
    } else {
        p.r = ((c & 0xf000000) >> 23) | ((c & 0xf000000) >> 27);
        p.g = ((c & 0xf00000) >> 19) | ((c & 0xf00000) >> 23);
        p.b = ((c & 0xf0000) >> 15) | ((c & 0xf0000) >> 19);
        p.a = (c & 0x70000000) >> 27;
    }
    return p;
}

static void interpolate(px_t P, px_t Q, px_t R, px_t S, px_t *out, int bpp) {
    int ww = bpp == 2 ? 8 : 4, wh = 4;
    px_t hP = P, hR = R;
    px_t QmP = { Q.r - P.r, Q.g - P.g, Q.b - P.b, Q.a - P.a };
    px_t SmR = { S.r - R.r, S.g - R.g, S.b - R.b, S.a - R.a };
    hP.r *= ww; hP.g *= ww; hP.b *= ww; hP.a *= ww;
    hR.r *= ww; hR.g *= ww; hR.b *= ww; hR.a *= ww;
    if (bpp == 2) {
        for (int x = 0; x < ww; x++) {
            px_t res = { 4 * hP.r, 4 * hP.g, 4 * hP.b, 4 * hP.a };
            px_t dY = { hR.r - hP.r, hR.g - hP.g, hR.b - hP.b, hR.a - hP.a };
            for (int y = 0; y < wh; y++) {
                px_t *o = &out[y * ww + x];
                o->r = (res.r >> 7) + (res.r >> 2);
                o->g = (res.g >> 7) + (res.g >> 2);
                o->b = (res.b >> 7) + (res.b >> 2);
                o->a = (res.a >> 5) + (res.a >> 1);
                res.r += dY.r; res.g += dY.g; res.b += dY.b; res.a += dY.a;
            }
            hP.r += QmP.r; hP.g += QmP.g; hP.b += QmP.b; hP.a += QmP.a;
            hR.r += SmR.r; hR.g += SmR.g; hR.b += SmR.b; hR.a += SmR.a;
        }
    } else {
        for (int y = 0; y < wh; y++) {
            px_t res = { 4 * hP.r, 4 * hP.g, 4 * hP.b, 4 * hP.a };
            px_t dY = { hR.r - hP.r, hR.g - hP.g, hR.b - hP.b, hR.a - hP.a };
            for (int x = 0; x < ww; x++) {
                px_t *o = &out[x * ww + y];
                o->r = (res.r >> 6) + (res.r >> 1);
                o->g = (res.g >> 6) + (res.g >> 1);
                o->b = (res.b >> 6) + (res.b >> 1);
                o->a = (res.a >> 4) + res.a;
                res.r += dY.r; res.g += dY.g; res.b += dY.b; res.a += dY.a;
            }
            hP.r += QmP.r; hP.g += QmP.g; hP.b += QmP.b; hP.a += QmP.a;
            hR.r += SmR.r; hR.g += SmR.g; hR.b += SmR.b; hR.a += SmR.a;
        }
    }
}

static void unpack_mod(word_t w, int ox, int oy, int mv[16][8], int mm[16][8], int bpp) {
    uint32_t mode = w.col & 1, bits = w.mod;
    if (bpp == 2) {
        if (mode) {
            if (bits & 1) {
                mode = (bits & (1u << 20)) ? 3 : 2;
                if (bits & (1u << 21)) bits |= (1u << 20); else bits &= ~(1u << 20);
            }
            if (bits & 2) bits |= 1; else bits &= ~1u;
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 8; x++) {
                    mm[x + ox][y + oy] = (int)mode;
                    if (((x ^ y) & 1) == 0) { mv[x + ox][y + oy] = bits & 3; bits >>= 2; }
                }
        } else {
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 8; x++) {
                    mm[x + ox][y + oy] = (int)mode;
                    mv[x + ox][y + oy] = (bits & 1) ? 3 : 0;
                    bits >>= 1;
                }
        }
    } else {
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                mm[x + ox][y + oy] = (int)mode;
                int v = bits & 3;
                if (mode) { if (v == 1) v = 4; else if (v == 2) v = 14; else if (v == 3) v = 8; }
                else { v *= 3; if (v > 3) v -= 1; }
                mv[x + ox][y + oy] = v;
                bits >>= 2;
            }
    }
}

static int mod_value(int mv[16][8], int mm[16][8], int x, int y, int bpp) {
    static const int rep[4] = { 0, 3, 5, 8 };
    if (bpp == 4) return mv[x][y];
    if (mm[x][y] == 0 || ((x ^ y) & 1) == 0) return rep[mv[x][y]];
    if (mm[x][y] == 1) return (rep[mv[x][y - 1]] + rep[mv[x][y + 1]] + rep[mv[x - 1][y]] + rep[mv[x + 1][y]] + 2) / 4;
    if (mm[x][y] == 2) return (rep[mv[x - 1][y]] + rep[mv[x + 1][y]] + 1) / 2;
    return (rep[mv[x][y - 1]] + rep[mv[x][y + 1]] + 1) / 2;
}

static uint32_t twiddle(uint32_t xs, uint32_t ys, uint32_t x, uint32_t y) {
    uint32_t mind = xs, maxv = y, tw = 0, sb = 1, db = 1;
    int sh = 0;
    if (ys < xs) { mind = ys; maxv = x; }
    while (sb < mind) {
        if (y & sb) tw |= db;
        if (x & sb) tw |= db << 1;
        sb <<= 1; db <<= 2; sh++;
    }
    maxv >>= sh;
    return tw | (maxv << (2 * sh));
}

static int wrapi(int n, int i) { return (i + n) % n; }

static void decode_padded(const uint32_t *words, uint8_t *out, int w, int h, int bpp) {
    int ww = bpp == 2 ? 8 : 4, wh = 4;
    int nx = w / ww, ny = h / wh;
    for (int wy = -1; wy < ny - 1; wy++)
        for (int wx = -1; wx < nx - 1; wx++) {
            int Px = wrapi(nx, wx), Py = wrapi(ny, wy), Qx = wrapi(nx, wx + 1), Qy = Py;
            int Rx = Px, Ry = wrapi(ny, wy + 1), Sx = Qx, Sy = Ry;
            uint32_t o[4] = { twiddle(nx, ny, Px, Py) * 2, twiddle(nx, ny, Qx, Qy) * 2,
                              twiddle(nx, ny, Rx, Ry) * 2, twiddle(nx, ny, Sx, Sy) * 2 };
            word_t W[4];
            for (int i = 0; i < 4; i++) { W[i].mod = words[o[i]]; W[i].col = words[o[i] + 1]; }
            int mv[16][8], mm[16][8];
            memset(mv, 0, sizeof(mv));
            memset(mm, 0, sizeof(mm));
            unpack_mod(W[0], 0, 0, mv, mm, bpp);
            unpack_mod(W[1], ww, 0, mv, mm, bpp);
            unpack_mod(W[2], 0, wh, mv, mm, bpp);
            unpack_mod(W[3], ww, wh, mv, mm, bpp);
            px_t ua[32], ub[32];
            interpolate(colour_a(W[0].col), colour_a(W[1].col), colour_a(W[2].col), colour_a(W[3].col), ua, bpp);
            interpolate(colour_b(W[0].col), colour_b(W[1].col), colour_b(W[2].col), colour_b(W[3].col), ub, bpp);
            uint8_t pix[32][4];
            for (int y = 0; y < wh; y++)
                for (int x = 0; x < ww; x++) {
                    int m = mod_value(mv, mm, x + ww / 2, y + wh / 2, bpp), punch = 0;
                    if (m > 10) { punch = 1; m -= 10; }
                    px_t A = ua[y * ww + x], B = ub[y * ww + x];
                    uint8_t *p = pix[y * ww + x];
                    p[0] = (uint8_t)((A.r * (8 - m) + B.r * m) / 8);
                    p[1] = (uint8_t)((A.g * (8 - m) + B.g * m) / 8);
                    p[2] = (uint8_t)((A.b * (8 - m) + B.b * m) / 8);
                    p[3] = punch ? 0 : (uint8_t)((A.a * (8 - m) + B.a * m) / 8);
                }
            for (int y = 0; y < wh / 2; y++)
                for (int x = 0; x < ww / 2; x++) {
                    memcpy(out + 4 * (((Py * wh) + y + wh / 2) * w + Px * ww + x + ww / 2), pix[y * ww + x], 4);
                    memcpy(out + 4 * (((Qy * wh) + y + wh / 2) * w + Qx * ww + x), pix[y * ww + x + ww / 2], 4);
                    memcpy(out + 4 * (((Ry * wh) + y) * w + Rx * ww + x + ww / 2), pix[(y + wh / 2) * ww + x], 4);
                    memcpy(out + 4 * (((Sy * wh) + y) * w + Sx * ww + x), pix[(y + wh / 2) * ww + x + ww / 2], 4);
                }
        }
}

uint8_t *pvrtc_decode(const void *data, int width, int height, int bpp) {
    if (width <= 0 || height <= 0 || (bpp != 2 && bpp != 4)) return NULL;
    int tw = width < (bpp == 2 ? 16 : 8) ? (bpp == 2 ? 16 : 8) : width;
    int th = height < 8 ? 8 : height;
    uint8_t *full = malloc((size_t)tw * th * 4);
    if (!full) return NULL;
    decode_padded((const uint32_t *)data, full, tw, th, bpp);
    if (tw == width && th == height) return full;
    uint8_t *out = malloc((size_t)width * height * 4);
    if (out)
        for (int y = 0; y < height; y++) memcpy(out + (size_t)y * width * 4, full + (size_t)y * tw * 4, (size_t)width * 4);
    free(full);
    return out;
}
