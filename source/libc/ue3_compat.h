#ifndef DM_UE3_COMPAT_H
#define DM_UE3_COMPAT_H

#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void *dm_dso_handle;                       /* __dso_handle for __cxa_atexit registrations */

long sysconf_soloader(int name);                  /* bionic _SC_* numbering */
int  sysinfo_soloader(void *si);                  /* bionic struct sysinfo */
int  uname_soloader(void *u);                     /* bionic struct utsname */
void assert_soloader(const char *file, int line, const char *expr);
int  isfinitef_soloader(float f);
int  isinff_soloader(float f);
void exit_noreturn_soloader(int status);          /* _exit */
int  pause_soloader(void);
int  sigprocmask_soloader(int how, const void *set, void *old);
int  utimes_soloader(const char *path, const void *times);
uint32_t inet_addr_soloader(const char *cp);
size_t wcsrtombs_soloader(char *dst, const wchar_t **src, size_t len, void *ps);
int  vfscanf_soloader(void *fp, const char *fmt, void *ap);   /* unsupported: always EOF */
int  svfscanf_soloader(void *fp, const char *fmt, void *ap);  /* unsupported: always EOF */
void *fgetwln_soloader(void *fp, size_t *len);                /* unsupported */
char *fparseln_soloader(void *fp, size_t *len, size_t *lineno, const char delim[3], int flags); /* unsupported */

#ifdef __cplusplus
}
#endif

#endif
