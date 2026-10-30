/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 * Copyright (C) 2026      Ellie J Turner
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/*
 * The engine's imports: every function and variable libUnrealEngine3.so takes from Android's libraries, mapped to
 * newlib, vitaGL, zlib, pthreads or the loader's own versions (the *_soloader functions in libc/ and elsewhere).
 * The table lists exactly the engine's imports, plus the names it looks up itself with dlsym (OpenSL ES).
 */
#include <ctype.h>
#include <inttypes.h>
#include <locale.h>
#include <malloc.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utime.h>
#include <wchar.h>
#include <wctype.h>
#include <zlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/unistd.h>
#include <so_util/so_util.h>
#include <psp2/kernel/clib.h>

#include "android/egl.h"
#include "audio/opensl.h"
#include "gfx/gl_fixes.h"
#include "gfx/glutil.h"
#include "libc/errno.h"
#include "libc/io.h"
#include "libc/log.h"
#include "libc/mem.h"
#include "libc/pthr.h"
#include "libc/sys.h"
#include "libc/time64.h"
#include "libc/ue3_compat.h"
#include "storage/io_stats.h"
#include "storage/mem_valve.h"
#include "storage/obb_stream.h"
#include "util/logger.h"
#include "util/utils.h"

const unsigned int __page_size = PAGE_SIZE;

extern void * _ZNSt9exceptionD2Ev;
extern void * _ZdaPv;
extern void * _ZdlPv;
extern void * _Znaj;
extern void *_Znwj;
extern void *__aeabi_atexit;
extern void *__aeabi_dadd;
extern void *__aeabi_i2d;
extern void *__aeabi_idiv;
extern void *__aeabi_idivmod;
extern void *__aeabi_l2d;
extern void *__aeabi_l2f;
extern void *__aeabi_ldivmod;
extern void *__aeabi_ui2d;
extern void *__aeabi_uidiv;
extern void *__aeabi_uidivmod;
extern void *__aeabi_ul2d;
extern void *__aeabi_ul2f;
extern void *__aeabi_uldivmod;
extern void *__aeabi_unwind_cpp_pr0;
extern void *__aeabi_unwind_cpp_pr1;
extern void *__cxa_call_unexpected;
extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;
extern void *__cxa_pure_virtual;
extern void *__gnu_ldivmod_helper;
extern void *__gnu_unwind_frame;
extern void *__srget;
extern void *__stack_chk_guard;
extern void *__swbuf;

extern const char *BIONIC_ctype_;
extern const short *BIONIC_toupper_tab_;

static FILE __sF_fake[3];

void *dlsym_soloader(void * handle, const char * symbol);

so_default_dynlib default_dynlib[] = {
        // Common C/C++ internals
        { "_ZNSt9exceptionD2Ev", (uintptr_t)&_ZNSt9exceptionD2Ev },
        { "_ZdaPv", (uintptr_t)&_ZdaPv },
        { "_ZdlPv", (uintptr_t)&_ZdlPv },
        { "_Znaj", (uintptr_t)&_Znaj },
        { "_Znwj", (uintptr_t)&_Znwj },
        { "__aeabi_atexit", (uintptr_t)&__aeabi_atexit },
        { "__aeabi_dadd", (uintptr_t)&__aeabi_dadd },
        { "__aeabi_i2d", (uintptr_t)&__aeabi_i2d },
        { "__aeabi_idiv", (uintptr_t)&__aeabi_idiv },
        { "__aeabi_idivmod", (uintptr_t)&__aeabi_idivmod },
        { "__aeabi_l2d", (uintptr_t)&__aeabi_l2d },
        { "__aeabi_l2f", (uintptr_t)&__aeabi_l2f },
        { "__aeabi_ldivmod", (uintptr_t)&__aeabi_ldivmod },
        { "__aeabi_ui2d", (uintptr_t)&__aeabi_ui2d },
        { "__aeabi_uidiv", (uintptr_t)&__aeabi_uidiv },
        { "__aeabi_uidivmod", (uintptr_t)&__aeabi_uidivmod },
        { "__aeabi_ul2d", (uintptr_t)&__aeabi_ul2d },
        { "__aeabi_ul2f", (uintptr_t)&__aeabi_ul2f },
        { "__aeabi_uldivmod", (uintptr_t)&__aeabi_uldivmod },
        { "__aeabi_unwind_cpp_pr0", (uintptr_t)&__aeabi_unwind_cpp_pr0 },
        { "__aeabi_unwind_cpp_pr1", (uintptr_t)&__aeabi_unwind_cpp_pr1 },
        { "__atomic_cmpxchg", (uintptr_t)&__atomic_cmpxchg },
        { "__atomic_dec", (uintptr_t)&__atomic_dec },
        { "__atomic_inc", (uintptr_t)&__atomic_inc },
        { "__atomic_swap", (uintptr_t)&__atomic_swap },
        { "__cxa_begin_cleanup", (uintptr_t)&ret0 },
        { "__cxa_call_unexpected", (uintptr_t)&__cxa_call_unexpected },
        { "__cxa_guard_acquire", (uintptr_t)&cxa_guard_acquire_tracked },
        { "__cxa_guard_release", (uintptr_t)&__cxa_guard_release },
        { "__cxa_pure_virtual", (uintptr_t)&__cxa_pure_virtual },
        { "__cxa_type_match", (uintptr_t)&ret0 },
        { "__gnu_Unwind_Find_exidx", (uintptr_t)&ret0 },
        { "__gnu_ldivmod_helper", (uintptr_t)&__gnu_ldivmod_helper },
        { "__gnu_unwind_frame", (uintptr_t)&__gnu_unwind_frame },
        { "__isinf", (uintptr_t)&ret0 },
        { "__page_size", (uintptr_t)&__page_size },
        { "__sF", (uintptr_t)&__sF_fake },
        { "__srget", (uintptr_t)&__srget },
        { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail_soloader },
        { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard },
        { "__swbuf", (uintptr_t)&__swbuf },
        { "__system_property_get", (uintptr_t)&__system_property_get_soloader },
        // ctype
        { "_ctype_", (uintptr_t)&BIONIC_ctype_ },
        { "_toupper_tab_", (uintptr_t)&BIONIC_toupper_tab_ },
        { "isalpha", (uintptr_t)&isalpha },
        // Android SDK standard logging
        { "__android_log_print", (uintptr_t)&__android_log_print },
        { "__android_log_write", (uintptr_t)&__android_log_write },
        // Math
        { "acosf", (uintptr_t)&acosf },
        { "asinf", (uintptr_t)&asinf },
        { "atan2f", (uintptr_t)&atan2f },
        { "atanf", (uintptr_t)&atanf },
        { "ceilf", (uintptr_t)&ceilf },
        { "cos", (uintptr_t)&cos },
        { "cosf", (uintptr_t)&cosf },
        { "expf", (uintptr_t)&expf },
        { "floor", (uintptr_t)&floor },
        { "floorf", (uintptr_t)&floorf },
        { "fmod", (uintptr_t)&fmod },
        { "fmodf", (uintptr_t)&fmodf },
        { "frexp", (uintptr_t)&frexp },
        { "ldexp", (uintptr_t)&ldexp },
        { "log", (uintptr_t)&log },
        { "log10f", (uintptr_t)&log10f },
        { "logf", (uintptr_t)&logf },
        { "pow", (uintptr_t)&pow },
        { "powf", (uintptr_t)&powf },
        { "roundf", (uintptr_t)&roundf },
        { "sin", (uintptr_t)&sin },
        { "sinf", (uintptr_t)&sinf },
        { "sqrt", (uintptr_t)&sqrt },
        { "sqrtf", (uintptr_t)&sqrtf },
        { "tanf", (uintptr_t)&tanf },
        // Sockets
        { "accept", (uintptr_t)&accept },
        { "bind", (uintptr_t)&bind },
        { "connect", (uintptr_t)&connect },
        { "freeaddrinfo", (uintptr_t)&freeaddrinfo },
        { "getaddrinfo", (uintptr_t)&getaddrinfo },
        { "gethostname", (uintptr_t)&gethostname },
        { "getsockname", (uintptr_t)&getsockname },
        { "getsockopt", (uintptr_t)&getsockopt },
        { "listen", (uintptr_t)&listen },
        { "recv", (uintptr_t)&recv },
        { "recvfrom", (uintptr_t)&recvfrom },
        { "select", (uintptr_t)&select },
        { "send", (uintptr_t)&send },
        { "sendto", (uintptr_t)&sendto },
        { "setsockopt", (uintptr_t)&setsockopt },
        { "socket", (uintptr_t)&socket },
        // Memory
        { "free", (uintptr_t)&free_valve },   /* storage/mem_valve.c, counted */
        { "malloc", (uintptr_t)&malloc_valve },
        { "memcmp", (uintptr_t)&memcmp },
        { "memcpy", (uintptr_t)&sceClibMemcpy },
        { "memmove", (uintptr_t)&memmove },
        { "memset", (uintptr_t)&memset },
        { "realloc", (uintptr_t)&realloc_valve },
        // IO
        { "close", (uintptr_t)&close_soloader },
        { "closedir", (uintptr_t)&closedir_soloader },
        { "fclose", (uintptr_t)&fclose_soloader },
        { "fcntl", (uintptr_t)&fcntl_soloader },
        { "fopen", (uintptr_t)&fopen_soloader },
        { "fstat", (uintptr_t)&fstat_soloader },
        { "ioctl", (uintptr_t)&ioctl_soloader },
        { "open", (uintptr_t)&open_soloader },
        { "opendir", (uintptr_t)&opendir_soloader },
        { "readdir", (uintptr_t)&readdir_soloader },
        { "stat", (uintptr_t)&stat_soloader },
        { "utime", (uintptr_t)&utime },
            { "feof", (uintptr_t)&feof },
            { "ferror", (uintptr_t)&ferror },
            { "fflush", (uintptr_t)&fflush },
            { "fgetc", (uintptr_t)&fgetc },
            { "fgetpos", (uintptr_t)&fgetpos },
            { "fgets", (uintptr_t)&fgets },
            { "fputc", (uintptr_t)&fputc },
            { "fputs", (uintptr_t)&fputs },
            { "fread", (uintptr_t)&fread },
            { "fseek", (uintptr_t)&fseek },
            { "ftell", (uintptr_t)&ftell },
            { "fwide", (uintptr_t)&fwide },
            { "fwrite", (uintptr_t)&fwrite },
            { "putchar", (uintptr_t)&putchar },
            { "setvbuf", (uintptr_t)&setvbuf },
            { "ungetc", (uintptr_t)&ungetc },
            { "ungetwc", (uintptr_t)&ungetwc },
        { "access", (uintptr_t)&access },
        { "chdir", (uintptr_t)&chdir },
        { "chmod", (uintptr_t)&chmod },
        { "dup", (uintptr_t)&dup_soloader },   /* virtual .obb descriptors, libc/io.c */
        { "ftruncate", (uintptr_t)&ftruncate_bionic },
        { "lseek", (uintptr_t)&lseek_bionic },   /* bionic's 32-bit off_t, see io_stats.c */
        { "lstat", (uintptr_t)&stat_soloader },   /* no symlinks on the Vita; and the engine needs the bionic layout */
        { "mkdir", (uintptr_t)&mkdir },
        { "read", (uintptr_t)&read_counted },
        { "remove", (uintptr_t)&remove_soloader },   /* waits for a save on its way */
        { "rename", (uintptr_t)&rename_soloader },
        { "rewind", (uintptr_t)&rewind },
        { "rmdir", (uintptr_t)&rmdir },
        { "unlink", (uintptr_t)&unlink_soloader },
        { "write", (uintptr_t)&write_soloader },
        // *printf, *scanf
        { "snprintf", (uintptr_t)&snprintf },
        { "sprintf", (uintptr_t)&sprintf },
        { "vprintf", (uintptr_t)&vprintf },
        { "vsnprintf", (uintptr_t)&vsnprintf },
        { "vsscanf", (uintptr_t)&vsscanf },
        { "vswprintf", (uintptr_t)&vswprintf },
            { "vfprintf", (uintptr_t)&vfprintf },
        // EGL
        { "eglGetDisplay", (uintptr_t)&eglGetDisplay },
        { "eglInitialize", (uintptr_t)&eglInitialize },
        // OpenGL
        { "glActiveTexture", (uintptr_t)&glActiveTexture },
        { "glAttachShader", (uintptr_t)&glAttachShader_soloader },
        { "glBindBuffer", (uintptr_t)&glBindBuffer },
        { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer_soloader },
        { "glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer },
        { "glBindTexture", (uintptr_t)&glBindTexture },
        { "glBlendColor", (uintptr_t)&ret0 },
        { "glBlendEquationSeparate", (uintptr_t)&glBlendEquationSeparate },
        { "glBlendFuncSeparate", (uintptr_t)&glBlendFuncSeparate },
        { "glBufferData", (uintptr_t)&glBufferData },
        { "glBufferSubData", (uintptr_t)&glBufferSubData },
        { "glClear", (uintptr_t)&glClear },
        { "glClearColor", (uintptr_t)&glClearColor },
        { "glClearDepthf", (uintptr_t)&glClearDepthf },
        { "glClearStencil", (uintptr_t)&glClearStencil },
        { "glColorMask", (uintptr_t)&glColorMask },
        { "glCompileShader", (uintptr_t)&glCompileShader_soloader },
        { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D_soloader },
        { "glCreateProgram", (uintptr_t)&glCreateProgram },
        { "glCreateShader", (uintptr_t)&glCreateShader },
        { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
        { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers },
        { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers },
        { "glDeleteTextures", (uintptr_t)&glDeleteTextures },
        { "glDepthFunc", (uintptr_t)&glDepthFunc },
        { "glDepthMask", (uintptr_t)&glDepthMask },
        { "glDepthRangef", (uintptr_t)&glDepthRangef },
        { "glDisable", (uintptr_t)&glDisable },
        { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray },
        { "glDrawArrays", (uintptr_t)&glDrawArrays_soloader },
        { "glDrawElements", (uintptr_t)&glDrawElements },
        { "glEnable", (uintptr_t)&glEnable },
        { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
        { "glFinish", (uintptr_t)&glFinish },
        { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer },
        { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D },
        { "glFrontFace", (uintptr_t)&glFrontFace },
        { "glGenBuffers", (uintptr_t)&glGenBuffers },
        { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
        { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers },
        { "glGenTextures", (uintptr_t)&glGenTextures },
        { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
        { "glGetError", (uintptr_t)&glGetError },
        { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
        { "glGetString", (uintptr_t)&glGetString_soloader },
        { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
        { "glLinkProgram", (uintptr_t)&glLinkProgram_soloader },
        { "glMapBufferOES", (uintptr_t)&glMapBuffer },
        { "glPixelStorei", (uintptr_t)&glPixelStorei_soloader },
        { "glPolygonOffset", (uintptr_t)&glPolygonOffset_soloader },
        { "glReadPixels", (uintptr_t)&glReadPixels },
        { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage },
        { "glScissor", (uintptr_t)&glScissor },
        { "glShaderSource", (uintptr_t)&glShaderSource_soloader },
        { "glStencilFunc", (uintptr_t)&glStencilFunc },
        { "glStencilFuncSeparate", (uintptr_t)&glStencilFuncSeparate },
        { "glStencilMask", (uintptr_t)&glStencilMask },
        { "glStencilOp", (uintptr_t)&glStencilOp },
        { "glStencilOpSeparate", (uintptr_t)&glStencilOpSeparate },
        { "glTexImage2D", (uintptr_t)&glTexImage2D_soloader },
        { "glTexParameteri", (uintptr_t)&glTexParameteri },
        { "glUniform1fv", (uintptr_t)&glUniform1fv },
        { "glUniform1i", (uintptr_t)&glUniform1i },
        { "glUniform2fv", (uintptr_t)&glUniform2fv },
        { "glUniform3fv", (uintptr_t)&glUniform3fv },
        { "glUniform4fv", (uintptr_t)&glUniform4fv },
        { "glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv },
        { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv },
        { "glUnmapBufferOES", (uintptr_t)&glUnmapBuffer },
        { "glUseProgram", (uintptr_t)&glUseProgram },
        { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer },
        { "glViewport", (uintptr_t)&glViewport },
        // OpenSL ES: the engine uses only slCreateEngine and the ENGINE/PLAY/VOLUME/BUFFERQUEUE IIDs, all from audio/opensl.c
        { "SL_IID_ENGINE", (uintptr_t)&dm_SL_IID_ENGINE },
        { "SL_IID_BUFFERQUEUE", (uintptr_t)&dm_SL_IID_BUFFERQUEUE },
        { "SL_IID_PLAY", (uintptr_t)&dm_SL_IID_PLAY },
        { "SL_IID_VOLUME", (uintptr_t)&dm_SL_IID_VOLUME },
        { "slCreateEngine", (uintptr_t)&dm_slCreateEngine },   /* the loader's own OpenSL ES: audio/opensl.c */
        // Pthread
        { "pthread_attr_init", (uintptr_t) &pthread_attr_init_soloader },
        { "pthread_attr_setstacksize", (uintptr_t) &pthread_attr_setstacksize_soloader },
        { "pthread_cond_broadcast", (uintptr_t) &pthread_cond_broadcast_soloader },
        { "pthread_cond_destroy", (uintptr_t) &pthread_cond_destroy_soloader },
        { "pthread_cond_init", (uintptr_t) &pthread_cond_init_soloader },
        { "pthread_cond_signal", (uintptr_t) &pthread_cond_signal_soloader },
        { "pthread_cond_timedwait", (uintptr_t) &pthread_cond_timedwait_soloader },
        { "pthread_cond_wait", (uintptr_t) &pthread_cond_wait_soloader },
        { "pthread_create", (uintptr_t) &pthread_create_soloader },
        { "pthread_detach", (uintptr_t) &pthread_detach_soloader },
        { "pthread_exit", (uintptr_t)&pthread_exit },
        { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
        { "pthread_join", (uintptr_t) &pthread_join_soloader },
        { "pthread_key_create", (uintptr_t)&pthread_key_create },
        { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
        { "pthread_mutex_destroy", (uintptr_t) &pthread_mutex_destroy_soloader },
        { "pthread_mutex_init", (uintptr_t) &pthread_mutex_init_soloader },
        { "pthread_mutex_lock", (uintptr_t) &pthread_mutex_lock_soloader },
        { "pthread_mutex_trylock", (uintptr_t) &pthread_mutex_trylock_soloader },
        { "pthread_mutex_unlock", (uintptr_t) &pthread_mutex_unlock_soloader },
        { "pthread_mutexattr_destroy", (uintptr_t) &pthread_mutexattr_destroy_soloader },
        { "pthread_mutexattr_init", (uintptr_t) &pthread_mutexattr_init_soloader },
        { "pthread_mutexattr_settype", (uintptr_t) &pthread_mutexattr_settype_soloader },
        { "pthread_once", (uintptr_t)&pthread_once_soloader },
        { "pthread_self", (uintptr_t) &pthread_self_soloader },
        { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
        { "sched_yield", (uintptr_t)&sched_yield_soloader },
        // wchar, wctype
        { "btowc", (uintptr_t)&btowc },
        { "iswalpha", (uintptr_t)&iswalpha },
        { "iswdigit", (uintptr_t)&iswdigit },
        { "iswdigit", (uintptr_t)&iswdigit },
        { "iswspace", (uintptr_t)&iswspace },
        { "iswupper", (uintptr_t)&iswupper },
        { "iswxdigit", (uintptr_t)&iswxdigit },
        { "mbrlen", (uintptr_t)&mbrlen },
        { "mbrtowc", (uintptr_t)&mbrtowc },
        { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
        { "wcrtomb", (uintptr_t)&wcrtomb },
        { "wcscpy", (uintptr_t)&wcscpy },
        { "wcslen", (uintptr_t)&wcslen },
        { "wctob", (uintptr_t)&wctob },
        { "wmemchr", (uintptr_t)&wmemchr },
        // libdl
        { "dlopen", (uintptr_t)&ret1 },
        { "dlsym", (uintptr_t)&dlsym_soloader },
        // Errno
        { "__errno", (uintptr_t)&__errno_soloader },
        { "strerror", (uintptr_t)&strerror_soloader },
        // Strings
        { "memchr", (uintptr_t)&memchr },
        { "strcasecmp", (uintptr_t)&strcasecmp },
        { "strcat", (uintptr_t)&strcat },
        { "strchr", (uintptr_t)&strchr },
        { "strcmp", (uintptr_t)&strcmp },
        { "strcoll", (uintptr_t)&strcoll },
        { "strcpy", (uintptr_t)&strcpy },
        { "strlen", (uintptr_t)&strlen },
        { "strncmp", (uintptr_t)&strncmp },
        { "strncpy", (uintptr_t)&strncpy },
        { "strrchr", (uintptr_t)&strrchr },
        { "strstr", (uintptr_t)&strstr },
        // Syscalls
        { "getpid", (uintptr_t)&getpid },
        { "syscall", (uintptr_t)&syscall },
        { "sysconf", (uintptr_t)&sysconf_soloader },
        // Time
        { "difftime", (uintptr_t)&difftime },
        { "gettimeofday", (uintptr_t)&gettimeofday },
        { "gmtime", (uintptr_t)&gmtime },
        { "gmtime_r", (uintptr_t)&gmtime_r },
        { "localtime", (uintptr_t)&localtime },
        { "localtime_r", (uintptr_t)&localtime_r },
        { "mktime", (uintptr_t)&mktime },
        { "time", (uintptr_t)&time },
        // stdlib
        { "abort", (uintptr_t)&abort_soloader },
        { "atoi", (uintptr_t)&atoi },
        { "exit", (uintptr_t)&exit_soloader },
        { "lrand48", (uintptr_t)&lrand48 },
        { "srand48", (uintptr_t)&srand48 },
        { "strtod", (uintptr_t)&strtod },
        { "strtoimax", (uintptr_t)&strtoimax },
        { "strtoumax", (uintptr_t)&strtoumax },
        { "usleep", (uintptr_t)&usleep_soloader },
        { "qsort", (uintptr_t)&qsort },
        // Signals
        { "bsd_signal", (uintptr_t)&signal },
        { "raise", (uintptr_t)&raise },
        { "sigaction", (uintptr_t)&sigaction },
        // zlib
        { "compress", (uintptr_t)&compress },
        { "crc32", (uintptr_t)&crc32 },
        { "gzclose", (uintptr_t)&gzclose },
        { "gzgets", (uintptr_t)&gzgets },
        { "gzopen", (uintptr_t)&gzopen },
        { "inflate", (uintptr_t)&inflate_timed },
        { "inflateEnd", (uintptr_t)&inflateEnd },
        { "inflateInit2_", (uintptr_t)&inflateInit2_ },
        { "uncompress", (uintptr_t)&uncompress_checked },
        { "get_crc_table", (uintptr_t)&get_crc_table },
        // Dark Meadow / UE3 additions (see libc/ue3_compat.c)
        { "__assert", (uintptr_t)&assert_soloader },
        { "__dso_handle", (uintptr_t)&dm_dso_handle },
        { "__isfinitef", (uintptr_t)&isfinitef_soloader },
        { "__isinff", (uintptr_t)&isinff_soloader },
        { "__svfscanf", (uintptr_t)&svfscanf_soloader },
        { "_exit", (uintptr_t)&exit_noreturn_soloader },
        { "_fgetwln", (uintptr_t)&fgetwln_soloader },
        { "_fileno", (uintptr_t)&obb_fileno },   /* expansion-file streams have no descriptor of their own */
        { "_fparseln", (uintptr_t)&fparseln_soloader },
        { "inet_addr", (uintptr_t)&inet_addr_soloader },
        { "isatty", (uintptr_t)&ret0 },
        { "iswalnum", (uintptr_t)&iswalnum },
        { "pause", (uintptr_t)&pause_soloader },
        { "sigprocmask", (uintptr_t)&sigprocmask_soloader },
        { "sysinfo", (uintptr_t)&sysinfo_soloader },
        { "uname", (uintptr_t)&uname_soloader },
        { "utimes", (uintptr_t)&utimes_soloader },
        { "vfscanf", (uintptr_t)&vfscanf_soloader },
        { "wcsrtombs", (uintptr_t)&wcsrtombs_soloader },
};

void *dlsym_soloader(void * handle, const char * symbol) {
    for (int i = 0; i < sizeof(default_dynlib) / sizeof(default_dynlib[0]); i++) {
        if (strcmp(symbol, default_dynlib[i].symbol) == 0) {
            return (void *)default_dynlib[i].func;
        }
    }

    l_error("dlsym: Unknown symbol \"%s\".", symbol);
    return NULL;
}

void resolve_imports(so_module* mod) {
    __sF_fake[0] = *stdin;
    __sF_fake[1] = *stdout;
    __sF_fake[2] = *stderr;

    so_resolve(mod, default_dynlib, sizeof(default_dynlib), 0);
}