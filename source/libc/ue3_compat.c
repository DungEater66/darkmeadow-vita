/*
 * libc/bionic functions libUnrealEngine3.so imports that the boilerplate's symbol table does not provide
 * (or provides as a stub that returns 0). Bionic numbering and struct layouts are used on purpose: the engine
 * was built against Android's headers.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "libc/ue3_compat.h"
#include "game_info.h"
#include "util/logger.h"

void *dm_dso_handle = NULL;

/* ---- sysconf / sysinfo / uname -------------------------------------------------------------- */

/* bionic (arm) values of the _SC_* constants the engine can plausibly ask for */
#define BIONIC_SC_PAGESIZE        0x0027
#define BIONIC_SC_NPROCESSORS_CONF 0x0060
#define BIONIC_SC_NPROCESSORS_ONLN 0x0061
#define BIONIC_SC_PHYS_PAGES      0x0062
#define BIONIC_SC_AVPHYS_PAGES    0x0063
#define BIONIC_SC_CLK_TCK         0x0002

long sysconf_soloader(int name) {
    switch (name) {
        case BIONIC_SC_PAGESIZE:          return 4096;
        case BIONIC_SC_NPROCESSORS_CONF:
        case BIONIC_SC_NPROCESSORS_ONLN:  return DM_CPU_CORES;
        case BIONIC_SC_PHYS_PAGES:        return (long)DM_TOTAL_RAM_MB * 256;   /* 1 MB = 256 pages */
        case BIONIC_SC_AVPHYS_PAGES:      return (long)DM_FREE_RAM_MB * 256;
        case BIONIC_SC_CLK_TCK:           return 100;
        default:
            l_warn("sysconf(0x%x): unknown name, returning -1.", name);
            errno = EINVAL;
            return -1;
    }
}

/* Linux/bionic struct sysinfo, 32-bit */
struct bionic_sysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    unsigned short procs, pad;
    unsigned long totalhigh, freehigh;
    unsigned int mem_unit;
    char _f[8];
};

int sysinfo_soloader(void *p) {
    struct bionic_sysinfo *si = p;
    if (!si) return -1;
    memset(si, 0, sizeof(*si));
    si->uptime = (long)(sceKernelGetProcessTimeWide() / 1000000);
    si->totalram = (unsigned long)DM_TOTAL_RAM_MB * 1024 * 1024;
    si->freeram = (unsigned long)DM_FREE_RAM_MB * 1024 * 1024;
    si->procs = 1;
    si->mem_unit = 1;
    return 0;
}

struct bionic_utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};

int uname_soloader(void *p) {
    struct bionic_utsname *u = p;
    if (!u) return -1;
    memset(u, 0, sizeof(*u));
    strcpy(u->sysname, "Linux");
    strcpy(u->nodename, "localhost");
    strcpy(u->release, "3.0.31");
    strcpy(u->version, "#1 SMP PREEMPT");
    strcpy(u->machine, "armv7l");
    strcpy(u->domainname, "(none)");
    return 0;
}

/* ---- process / signals / misc -------------------------------------------------------------- */

void assert_soloader(const char *file, int line, const char *expr) {
    l_fatal("Engine assertion failed: %s (%s:%d)", expr ? expr : "?", file ? file : "?", line);
    sceKernelExitProcess(1);
}

void exit_noreturn_soloader(int status) { sceKernelExitProcess(status); }

int pause_soloader(void) {
    for (;;) sceKernelDelayThread(1000 * 1000);
    return -1;
}

int sigprocmask_soloader(int how, const void *set, void *old) {
    if (old) memset(old, 0, sizeof(unsigned long));
    return 0;
}

int utimes_soloader(const char *path, const void *times) { return 0; }

int isfinitef_soloader(float f) { return isfinite(f); }
int isinff_soloader(float f)    { return isinf(f) ? (f > 0.f ? 1 : -1) : 0; }

/* inet_addr: dotted quad -> network byte order, INADDR_NONE (0xFFFFFFFF) on error */
uint32_t inet_addr_soloader(const char *cp) {
    unsigned a, b, c, d;
    char tail;
    if (!cp || sscanf(cp, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return 0xFFFFFFFFu;
    return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);   /* little-endian host */
}

/* ---- wide chars ------------------------------------------------------------------------------ */

/* wchar_t is 4 bytes on Android as well; encode as UTF-8. dst == NULL only counts the bytes. */
size_t wcsrtombs_soloader(char *dst, const wchar_t **src, size_t len, void *ps) {
    const wchar_t *s = *src;
    size_t n = 0;
    for (;; s++) {
        uint32_t c = (uint32_t)*s;
        char tmp[4];
        size_t k;
        if (c < 0x80)          { tmp[0] = (char)c; k = 1; }
        else if (c < 0x800)    { tmp[0] = 0xC0 | (c >> 6);  tmp[1] = 0x80 | (c & 0x3F); k = 2; }
        else if (c < 0x10000)  { tmp[0] = 0xE0 | (c >> 12); tmp[1] = 0x80 | ((c >> 6) & 0x3F); tmp[2] = 0x80 | (c & 0x3F); k = 3; }
        else                   { tmp[0] = 0xF0 | (c >> 18); tmp[1] = 0x80 | ((c >> 12) & 0x3F); tmp[2] = 0x80 | ((c >> 6) & 0x3F); tmp[3] = 0x80 | (c & 0x3F); k = 4; }
        if (dst) {
            if (n + k > len) { *src = s; return n; }
            memcpy(dst + n, tmp, k);
        }
        if (c == 0) { if (dst) *src = NULL; return n; }   /* terminator is written but not counted */
        n += k;
    }
}

/* ---- stdio extras the engine links but (so far) never needs ------------------------------- */

static void unsupported_once(const char *what, int *flag) {
    if (!*flag) { *flag = 1; l_error("%s is not supported by the loader (engine called it).", what); }
}
int vfscanf_soloader(void *fp, const char *fmt, void *ap)  { static int f; unsupported_once("vfscanf", &f);  return -1; }
int svfscanf_soloader(void *fp, const char *fmt, void *ap) { static int f; unsupported_once("__svfscanf", &f); return -1; }
void *fgetwln_soloader(void *fp, size_t *len)              { static int f; unsupported_once("_fgetwln", &f);  return NULL; }
char *fparseln_soloader(void *fp, size_t *len, size_t *lineno, const char delim[3], int flags) {
    static int f; unsupported_once("_fparseln", &f); return NULL;
}
