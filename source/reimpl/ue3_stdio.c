#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <so_util/so_util.h>

#include "reimpl/ue3_stdio.h"
#include "reimpl/io_stats.h"
#include "utils/logger.h"

extern so_module so_mod;

/* The engine's own stdin/stdout/stderr: three bionic FILE structs (84 bytes each) in its __sF array. */
static uintptr_t sf_base;
#define SF_ENTRY_SIZE 84
#define SF_COUNT 3

static int is_std(const FILE *fp) {
    return sf_base && (uintptr_t)fp >= sf_base && (uintptr_t)fp < sf_base + SF_COUNT * SF_ENTRY_SIZE;
}
static int is_std_out(const FILE *fp) { return is_std(fp) && (uintptr_t)fp >= sf_base + SF_ENTRY_SIZE; }

static void std_write(const FILE *fp, const char *s, size_t n) {
    if (is_std_out(fp) && n) l_debug("[engine stdio] %.*s", (int)n, s);
}

/* ---- hooks: same names/signatures as the bionic functions they replace ---------------------------------------- */

static int h_fclose(FILE *fp)                { return is_std(fp) ? 0 : fclose(fp); }
static int h_feof(FILE *fp)                  { return is_std(fp) ? 0 : feof(fp); }
static int h_ferror(FILE *fp)                { return is_std(fp) ? 0 : ferror(fp); }
static int h_fflush(FILE *fp)                { return (fp && is_std(fp)) ? 0 : fflush(fp); }
static void h_clearerr(FILE *fp)             { if (!is_std(fp)) clearerr(fp); }
static void h_rewind(FILE *fp)               { if (!is_std(fp)) rewind(fp); }

static int h_fgetc(FILE *fp)                 { return is_std(fp) ? EOF : fgetc(fp); }
static int h_getc(FILE *fp)                  { return is_std(fp) ? EOF : fgetc(fp); }
static int h_srget(FILE *fp)                 { return is_std(fp) ? EOF : fgetc(fp); }
static int h_ungetc(int c, FILE *fp)         { return is_std(fp) ? EOF : ungetc(c, fp); }
static char *h_fgets(char *s, int n, FILE *fp) { return is_std(fp) ? NULL : fgets(s, n, fp); }
static size_t h_fread(void *p, size_t sz, size_t n, FILE *fp) {
    if (is_std(fp)) return 0;
    size_t r = fread(p, sz, n, fp);
    io_stats_note(r * sz, __builtin_return_address(0));
    return r;
}

static int h_fputc(int c, FILE *fp)          { if (is_std(fp)) { char ch = (char)c; std_write(fp, &ch, 1); return c; } return fputc(c, fp); }
static int h_putc(int c, FILE *fp)           { return h_fputc(c, fp); }
static int h_swbuf(int c, FILE *fp)          { return h_fputc(c, fp); }
static int h_fputs(const char *s, FILE *fp)  { if (is_std(fp)) { std_write(fp, s, strlen(s)); return 0; } return fputs(s, fp); }
static size_t h_fwrite(const void *p, size_t sz, size_t n, FILE *fp) {
    if (is_std(fp)) { std_write(fp, p, sz * n); return n; }
    return fwrite(p, sz, n, fp);
}

/* bionic's off_t / fpos_t are 32-bit on ARM */
static int h_fseek(FILE *fp, long off, int whence)   { return is_std(fp) ? -1 : fseek(fp, off, whence); }
static long h_ftell(FILE *fp)                        { return is_std(fp) ? -1 : ftell(fp); }
static int h_fgetpos(FILE *fp, void *pos) {
    if (is_std(fp)) return -1;
    long p = ftell(fp);
    if (p < 0) return -1;
    *(int32_t *)pos = (int32_t)p;
    return 0;
}

static void h_setbuf(FILE *fp, char *buf)            { if (!is_std(fp)) setbuf(fp, buf); }
static void h_setlinebuf(FILE *fp)                   { if (!is_std(fp)) setvbuf(fp, NULL, _IOLBF, 0); }

static int h_fprintf(FILE *fp, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r;
    if (is_std(fp)) {
        char buf[512];
        r = vsnprintf(buf, sizeof(buf), fmt, ap);
        std_write(fp, buf, r < 0 ? 0 : (r < (int)sizeof(buf) ? (size_t)r : sizeof(buf) - 1));
    } else {
        r = vfprintf(fp, fmt, ap);
    }
    va_end(ap);
    return r;
}

static int h_fscanf(FILE *fp, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = is_std(fp) ? EOF : vfscanf(fp, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- installation ---------------------------------------------------------------------------------------------- */

static const struct { const char *name; uintptr_t fn; } hooks[] = {
    { "fclose",    (uintptr_t)&h_fclose },    { "feof",     (uintptr_t)&h_feof },
    { "ferror",    (uintptr_t)&h_ferror },    { "fflush",   (uintptr_t)&h_fflush },
    { "clearerr",  (uintptr_t)&h_clearerr },  { "rewind",   (uintptr_t)&h_rewind },
    { "fgetc",     (uintptr_t)&h_fgetc },     { "getc",     (uintptr_t)&h_getc },
    { "__srget",   (uintptr_t)&h_srget },     { "ungetc",   (uintptr_t)&h_ungetc },
    { "fgets",     (uintptr_t)&h_fgets },     { "fread",    (uintptr_t)&h_fread },
    { "fputc",     (uintptr_t)&h_fputc },     { "putc",     (uintptr_t)&h_putc },
    { "__swbuf",   (uintptr_t)&h_swbuf },     { "fputs",    (uintptr_t)&h_fputs },
    { "fwrite",    (uintptr_t)&h_fwrite },    { "fseek",    (uintptr_t)&h_fseek },
    { "fseeko",    (uintptr_t)&h_fseek },     /* same code as _fseeko */
    { "ftell",     (uintptr_t)&h_ftell },     { "ftello",   (uintptr_t)&h_ftell },   /* same code as _ftello */
    { "fgetpos",   (uintptr_t)&h_fgetpos },   { "setbuf",   (uintptr_t)&h_setbuf },
    { "setlinebuf",(uintptr_t)&h_setlinebuf },{ "fprintf",  (uintptr_t)&h_fprintf },
    { "fscanf",    (uintptr_t)&h_fscanf },
};

void ue3_stdio_hook_all(void) {
    sf_base = (uintptr_t)so_symbol(&so_mod, "__sF");
    if (!sf_base) l_warn("Engine __sF not found; its stdin/stdout/stderr objects will not be recognised.");

    int n = 0;
    for (unsigned i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
        uintptr_t addr = (uintptr_t)so_symbol(&so_mod, hooks[i].name);
        if (!addr) {
            l_warn("Engine stdio function \"%s\" not found, not hooked.", hooks[i].name);
            continue;
        }
        hook_addr(addr, hooks[i].fn);
        n++;
    }
    l_info("Hooked %d of %d engine stdio functions.", n, (int)(sizeof(hooks) / sizeof(hooks[0])));
}
