/* imports.c -- .so import resolution for libff3.so (FF3 3D)
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * The Cuore engine self-contains its C++ runtime (no std::/_Znwm/__cxa_throw
 * imports), so FF3 3D uses a single module. The dynamic imports
 * are: a libc subset (shimmed where bionic/newlib differ), GLES1 fixed-function
 * (mesa libGLESv1_CM), OpenSLES (our opensles.c shim) and the AAsset NDK API.
 */

#define _GNU_SOURCE // vasprintf, stpcpy, strncasecmp and friends

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <wchar.h>
#include <errno.h>
#include <sys/time.h>
#include <GLES/gl.h>
#include <switch.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "opensles.h"

extern uintptr_t __cxa_atexit;
extern uintptr_t __stack_chk_fail;

static uint64_t __stack_chk_guard_fake = 0x4242424242424242;

FILE *stderr_fake = (FILE *)0x1337;

void __assert2(const char *file, int line, const char *func, const char *expr) {
  debugPrintf("assertion failed:\n%s:%d (%s): %s\n", file, line, func, expr);
  abort();
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
#if DEBUG_LOG
  va_list list;
  static char string[0x1000];
  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);
  debugPrintf("%s: %s\n", tag, string);
#else
  (void)tag; (void)fmt;
#endif
  return 0;
}

int __android_log_write(int prio, const char *tag, const char *text) {
  (void)prio;
  debugPrintf("%s: %s\n", tag, text);
  return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list va) {
  (void)prio;
#if DEBUG_LOG
  static char string[0x1000];
  vsnprintf(string, sizeof(string), fmt, va);
  debugPrintf("%s: %s\n", tag, string);
#else
  (void)tag; (void)fmt; (void)va;
#endif
  return 0;
}

// ---------------------------------------------------------------------------
// pthread wrappers: bionic allocates the opaque types inline and zero-inits
// them, so we lazily back them with heap-allocated newlib objects stashed
// through the caller's pointer slot.
// ---------------------------------------------------------------------------

int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *mutexattr) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return -1;
  const int recursive = (mutexattr && *mutexattr == 1);
  // create a genuinely recursive newlib mutex via the real attr API -- the SQEX
  // sound mutex is recursive and re-locked on the same thread (NNS_SndUpdate).
  // (the previous code set the static recursive value then clobbered it with
  // pthread_mutex_init(m, NULL), silently making it non-recursive -> deadlock.)
  int ret;
  if (recursive) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    ret = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
  } else {
    ret = pthread_mutex_init(m, NULL);
  }
  if (ret != 0) { free(m); return -1; }
  *uid = m;
  return 0;
}

int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && *uid && (uintptr_t)*uid > 0x8000) {
    pthread_mutex_destroy(*uid);
    free(*uid);
    *uid = NULL;
  }
  return 0;
}

int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  int ret = 0;
  if (!*uid) ret = pthread_mutex_init_fake(uid, NULL);
  else if ((uintptr_t)*uid == 0x4000) { int attr = 1; ret = pthread_mutex_init_fake(uid, &attr); }
  if (ret < 0) return ret;
  return pthread_mutex_lock(*uid);
}

int pthread_mutex_trylock_fake(pthread_mutex_t **uid) {
  int ret = 0;
  if (!*uid) ret = pthread_mutex_init_fake(uid, NULL);
  else if ((uintptr_t)*uid == 0x4000) { int attr = 1; ret = pthread_mutex_init_fake(uid, &attr); }
  if (ret < 0) return ret;
  return pthread_mutex_trylock(*uid);
}

int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  int ret = 0;
  if (!*uid) ret = pthread_mutex_init_fake(uid, NULL);
  else if ((uintptr_t)*uid == 0x4000) { int attr = 1; ret = pthread_mutex_init_fake(uid, &attr); }
  if (ret < 0) return ret;
  return pthread_mutex_unlock(*uid);
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *condattr) {
  (void)condattr;
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return -1;
  *c = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
  if (pthread_cond_init(c, NULL) < 0) { free(c); return -1; }
  *cnd = c;
  return 0;
}

int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  return pthread_cond_broadcast(*cnd);
}

int pthread_cond_signal_fake(pthread_cond_t **cnd) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  return pthread_cond_signal(*cnd);
}

int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  if (cnd && *cnd) {
    pthread_cond_destroy(*cnd);
    free(*cnd);
    *cnd = NULL;
  }
  return 0;
}

int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  return pthread_cond_wait(*cnd, *mtx);
}

int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  return pthread_cond_timedwait(*cnd, *mtx, t);
}

int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine) return -1;
  if (__sync_lock_test_and_set(once_control, 1) == 0)
    (*init_routine)();
  return 0;
}

// engine threads must also get tpidr_el0 pointed at a stack-guard block before
// they run any guarded engine code (see tls_setup_guard)
typedef struct { void *(*entry)(void *); void *arg; } ThreadStart;

static void *thread_trampoline(void *p) {
  ThreadStart ts = *(ThreadStart *)p;
  free(p);
  tls_setup_guard();
  return ts.entry(ts.arg);
}

int pthread_create_fake(pthread_t *thread, const void *unused, void *entry, void *arg) {
  (void)unused;
  ThreadStart *ts = malloc(sizeof(*ts));
  if (!ts)
    return -1;
  ts->entry = (void *(*)(void *))entry;
  ts->arg = arg;
  return pthread_create(thread, NULL, thread_trampoline, ts);
}

// ---------------------------------------------------------------------------
// small misc shims
// ---------------------------------------------------------------------------

static int getpid_fake(void) { return 1; }

static int sched_yield_fake(void) { svcSleepThread(0); return 0; }

static void sincos_fake(double x, double *s, double *c) {
  if (s) *s = sin(x);
  if (c) *c = cos(x);
}

// bionic pthread_mutexattr_t is an int. We store the type plainly and read it
// back in pthread_mutex_init_fake. This MUST be functional: the SQEX sound
// system creates a RECURSIVE mutex via init+settype and re-locks it from the
// same thread (NNS_SndUpdate); a non-recursive mutex self-deadlocks.
// (PTHREAD_MUTEX_RECURSIVE == 1 in bionic.)
int pthread_mutexattr_init_fake(int *attr) { if (attr) *attr = 0; return 0; }
int pthread_mutexattr_settype_fake(int *attr, int type) { if (attr) *attr = type; return 0; }

char *__strrchr_chk_fake(const char *s, int c, size_t slen) {
  (void)slen;
  return strrchr(s, c);
}

// DEBUG: snapshot the GL light/material/fog state on the first few lit (3D) draws.
#if DEBUG_LOG
static void gl_dump_lit_state(void) {
  static int n = 0;
  if (n >= 6) return;
  if (!glIsEnabled(GL_LIGHTING)) return;  // only character/lit draws
  n++;
  GLfloat la[4]={0}, ld[4]={0}, lp[4]={0}, ma[4]={0}, md[4]={0}, me[4]={0}, ms[4]={0}, lma[4]={0}, fc[4]={0}, cc[4]={0};
  glGetLightfv(GL_LIGHT0, GL_AMBIENT, la);
  glGetLightfv(GL_LIGHT0, GL_DIFFUSE, ld);
  glGetLightfv(GL_LIGHT0, GL_POSITION, lp);
  glGetMaterialfv(GL_FRONT, GL_AMBIENT, ma);
  glGetMaterialfv(GL_FRONT, GL_DIFFUSE, md);
  glGetMaterialfv(GL_FRONT, GL_EMISSION, me);
  glGetMaterialfv(GL_FRONT, GL_SPECULAR, ms);
  glGetFloatv(GL_LIGHT_MODEL_AMBIENT, lma);
  glGetFloatv(GL_FOG_COLOR, fc);
  glGetFloatv(GL_CURRENT_COLOR, cc);
  debugPrintf("LIT#%d L0en=%d amb(%.2f %.2f %.2f) dif(%.2f %.2f %.2f) pos(%.1f %.1f %.1f %.0f)\n",
    n, glIsEnabled(GL_LIGHT0), la[0],la[1],la[2], ld[0],ld[1],ld[2], lp[0],lp[1],lp[2],lp[3]);
  debugPrintf("  mat amb(%.2f %.2f %.2f) dif(%.2f %.2f %.2f) emi(%.2f %.2f %.2f) spc(%.2f %.2f %.2f) lmAmb(%.2f %.2f %.2f)\n",
    ma[0],ma[1],ma[2], md[0],md[1],md[2], me[0],me[1],me[2], ms[0],ms[1],ms[2], lma[0],lma[1],lma[2]);
  debugPrintf("  L1en=%d L2en=%d L3en=%d fog=%d fogcol(%.2f %.2f %.2f) cur(%.2f %.2f %.2f %.2f) colMat=%d norm=%d tex=%d\n",
    glIsEnabled(GL_LIGHT1), glIsEnabled(GL_LIGHT2), glIsEnabled(GL_LIGHT3), glIsEnabled(GL_FOG),
    fc[0],fc[1],fc[2], cc[0],cc[1],cc[2],cc[3],
    glIsEnabled(GL_COLOR_MATERIAL), glIsEnabled(GL_NORMALIZE), glIsEnabled(GL_TEXTURE_2D));
}
static void glDrawArrays_log(GLenum mode, GLint first, GLsizei count) {
  gl_dump_lit_state();
  glDrawArrays(mode, first, count);
}
static void glColor4ub_log(GLubyte r, GLubyte g, GLubyte b, GLubyte a) {
  static unsigned last = 0xffffffffu; static int lines = 0;
  unsigned v = (r<<24)|(g<<16)|(b<<8)|a;
  if (v != last && lines < 20) { last = v; lines++; debugPrintf("glColor4ub(%d %d %d %d)\n", r,g,b,a); }
  glColor4ub(r, g, b, a);
}
#endif

static int gl_is_white_emission(const GLfloat *p) {
  return p && p[0] > 0.98f && p[1] > 0.98f && p[2] > 0.98f;
}

static int gl_rgb_has_value(const GLfloat *p) {
  return p && (p[0] > 0.05f || p[1] > 0.05f || p[2] > 0.05f);
}

static int gl_material_has_lit_color(GLenum face) {
  GLfloat ambient[4] = {0};
  GLfloat diffuse[4] = {0};
  GLenum query_face = face == GL_BACK ? GL_BACK : GL_FRONT;

  glGetMaterialfv(query_face, GL_AMBIENT, ambient);
  glGetMaterialfv(query_face, GL_DIFFUSE, diffuse);
  return gl_rgb_has_value(ambient) || gl_rgb_has_value(diffuse);
}

static void glMaterialfv_fix(GLenum face, GLenum pname, const GLfloat *p) {
  static const GLfloat black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

  if (pname == GL_EMISSION && gl_is_white_emission(p) &&
      gl_material_has_lit_color(face)) {
    glMaterialfv(face, pname, black);
    return;
  }

  glMaterialfv(face, pname, p);
}


// import table -------------------------------------------------------------

DynLibFunction dynlib_functions[] = {
  { "__sF", (uintptr_t)&fake_sF },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__errno", (uintptr_t)&__errno },
  { "__assert2", (uintptr_t)&__assert2 },

  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
  { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },

  // fortify wrappers (ignore the object-size argument)
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_fake },
  { "__memmove_chk", (uintptr_t)&__memmove_chk_fake },
  { "__strcat_chk", (uintptr_t)&__strcat_chk_fake },
  { "__strchr_chk", (uintptr_t)&__strchr_chk_fake },
  { "__strcpy_chk", (uintptr_t)&__strcpy_chk_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__strncpy_chk", (uintptr_t)&__strncpy_chk_fake },
  { "__strncpy_chk2", (uintptr_t)&__strncpy_chk2_fake },
  { "__strrchr_chk", (uintptr_t)&__strrchr_chk_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__vsprintf_chk", (uintptr_t)&__vsprintf_chk_fake },

  // bionic misc
  { "__android_log_print", (uintptr_t)&__android_log_print },
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "syscall", (uintptr_t)&syscall_fake },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "openlog", (uintptr_t)&ret0 },
  { "closelog", (uintptr_t)&ret0 },
  { "syslog", (uintptr_t)&ret0 },
  { "abort", (uintptr_t)&abort },

  // AAsset NDK API (emulated over the OBB / loose files, see libc_shim.c)
  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake },
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open_fake },
  { "AAsset_close", (uintptr_t)&AAsset_close_fake },
  { "AAsset_getLength", (uintptr_t)&AAsset_getLength_fake },
  { "AAsset_read", (uintptr_t)&AAsset_read_fake },
  { "AAsset_seek", (uintptr_t)&AAsset_seek_fake },

  // memory
  { "malloc", (uintptr_t)&malloc },
  { "calloc", (uintptr_t)&calloc },
  { "realloc", (uintptr_t)&realloc },
  { "free", (uintptr_t)&free },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },

  // mem/str
  { "memchr", (uintptr_t)&memchr },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "strcat", (uintptr_t)&strcat },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcpy", (uintptr_t)&strcpy },
  { "strlen", (uintptr_t)&strlen },
  { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy },
  { "strrchr", (uintptr_t)&strrchr },
  { "strstr", (uintptr_t)&strstr },
  { "stpcpy", (uintptr_t)&stpcpy },
  { "strtok", (uintptr_t)&strtok },
  { "strtod", (uintptr_t)&strtod },
  { "strtof", (uintptr_t)&strtof },
  { "strtol", (uintptr_t)&strtol },
  { "strtold", (uintptr_t)&strtold },
  { "strtoll", (uintptr_t)&strtoll },
  { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull },
  { "atoi", (uintptr_t)&atoi },
  { "atof", (uintptr_t)&atof },
  { "toupper", (uintptr_t)&toupper },
  { "qsort", (uintptr_t)&qsort },
  { "rand", (uintptr_t)&rand },
  { "srand", (uintptr_t)&srand },

  // wide char
  { "wcslen", (uintptr_t)&wcslen },
  { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof },
  { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold },
  { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul },
  { "wcstoull", (uintptr_t)&wcstoull },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },

  // printf family
  { "printf", (uintptr_t)&debugPrintf },
  { "puts", (uintptr_t)&puts },
  { "snprintf", (uintptr_t)&snprintf },
  { "sprintf", (uintptr_t)&sprintf },
  { "swprintf", (uintptr_t)&swprintf },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf },
  { "vasprintf", (uintptr_t)&vasprintf },

  // math
  { "atan2f", (uintptr_t)&atan2f },
  { "atanf", (uintptr_t)&atanf },
  { "cos", (uintptr_t)&cos },
  { "cosf", (uintptr_t)&cosf },
  { "sin", (uintptr_t)&sin },
  { "sinf", (uintptr_t)&sinf },
  { "sincos", (uintptr_t)&sincos_fake },
  { "sincosf", (uintptr_t)&sincosf_fake },
  { "sqrtf", (uintptr_t)&sqrtf },
  { "tanf", (uintptr_t)&tanf },

  // time
  { "gettimeofday", (uintptr_t)&gettimeofday },
  { "gmtime", (uintptr_t)&gmtime },
  { "localtime", (uintptr_t)&localtime },
  { "time", (uintptr_t)&time },
  { "usleep", (uintptr_t)&usleep },

  // stdio (over the fake bionic __sF and buffered fopen)
  { "fopen", (uintptr_t)&fopen_fake },
  { "fclose", (uintptr_t)&fclose_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "fseek", (uintptr_t)&fseek_fake },
  { "ftell", (uintptr_t)&ftell },
  { "fflush", (uintptr_t)&fflush_fake },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "fputc", (uintptr_t)&fputc_fake },
  { "vfprintf", (uintptr_t)&vfprintf_fake },

  // pthread
  { "pthread_create", (uintptr_t)&pthread_create_fake },
  { "pthread_join", (uintptr_t)&pthread_join },
  { "pthread_key_create", (uintptr_t)&pthread_key_create },
  { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
  { "pthread_once", (uintptr_t)&pthread_once_fake },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake },
  { "pthread_mutexattr_destroy", (uintptr_t)&ret0 },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake },
  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake },

  // misc extras
  { "getpid", (uintptr_t)&getpid_fake },
  { "sched_yield", (uintptr_t)&sched_yield_fake },

  // GLES1 fixed-function pipeline (mesa libGLESv1_CM)
  { "glAlphaFunc", (uintptr_t)&glAlphaFunc },
  { "glBindTexture", (uintptr_t)&glBindTexture },
  { "glBlendFunc", (uintptr_t)&glBlendFunc },
  { "glClear", (uintptr_t)&glClear },
  { "glClearColor", (uintptr_t)&glClearColor },
#if DEBUG_LOG
  { "glColor4ub", (uintptr_t)&glColor4ub_log },
#else
  { "glColor4ub", (uintptr_t)&glColor4ub },
#endif
  { "glColorPointer", (uintptr_t)&glColorPointer },
  { "glCullFace", (uintptr_t)&glCullFace },
  { "glDeleteTextures", (uintptr_t)&glDeleteTextures },
  { "glDepthFunc", (uintptr_t)&glDepthFunc },
  { "glDepthMask", (uintptr_t)&glDepthMask },
  { "glDisable", (uintptr_t)&glDisable },
  { "glDisableClientState", (uintptr_t)&glDisableClientState },
#if DEBUG_LOG
  { "glDrawArrays", (uintptr_t)&glDrawArrays_log },
#else
  { "glDrawArrays", (uintptr_t)&glDrawArrays },
#endif
  { "glEnable", (uintptr_t)&glEnable },
  { "glEnableClientState", (uintptr_t)&glEnableClientState },
  { "glFogf", (uintptr_t)&glFogf },
  { "glFogfv", (uintptr_t)&glFogfv },
  { "glGenTextures", (uintptr_t)&glGenTextures },
  { "glLightfv", (uintptr_t)&glLightfv },
  { "glLoadIdentity", (uintptr_t)&glLoadIdentity },
  { "glLoadMatrixf", (uintptr_t)&glLoadMatrixf },
  { "glMaterialfv", (uintptr_t)&glMaterialfv_fix },
  { "glMatrixMode", (uintptr_t)&glMatrixMode },
  { "glMultMatrixf", (uintptr_t)&glMultMatrixf },
  { "glNormalPointer", (uintptr_t)&glNormalPointer },
  { "glOrthof", (uintptr_t)&glOrthof },
  { "glPopMatrix", (uintptr_t)&glPopMatrix },
  { "glPushMatrix", (uintptr_t)&glPushMatrix },
  { "glScissor", (uintptr_t)&glScissor },
  { "glTexCoordPointer", (uintptr_t)&glTexCoordPointer },
  { "glTexImage2D", (uintptr_t)&glTexImage2D },
  { "glTexParameteri", (uintptr_t)&glTexParameteri },
  { "glTexSubImage2D", (uintptr_t)&glTexSubImage2D },
  { "glTranslatef", (uintptr_t)&glTranslatef },
  { "glVertexPointer", (uintptr_t)&glVertexPointer },
  { "glViewport", (uintptr_t)&glViewport },

  // OpenSLES (minimal SDL2-backed shim, see opensles.c)
  { "slCreateEngine", (uintptr_t)&slCreateEngine },
  #define SL_IID(n) { "SL_IID_" #n, (uintptr_t)&SL_IID_##n }
  SL_IID(3DCOMMIT), SL_IID(3DDOPPLER), SL_IID(3DGROUPING), SL_IID(3DLOCATION),
  SL_IID(3DMACROSCOPIC), SL_IID(3DSOURCE), SL_IID(ANDROIDCONFIGURATION),
  SL_IID(ANDROIDEFFECT), SL_IID(ANDROIDEFFECTCAPABILITIES), SL_IID(ANDROIDEFFECTSEND),
  SL_IID(ANDROIDSIMPLEBUFFERQUEUE), SL_IID(AUDIODECODERCAPABILITIES), SL_IID(AUDIOENCODER),
  SL_IID(AUDIOENCODERCAPABILITIES), SL_IID(AUDIOIODEVICECAPABILITIES), SL_IID(BASSBOOST),
  SL_IID(BUFFERQUEUE), SL_IID(DEVICEVOLUME), SL_IID(DYNAMICINTERFACEMANAGEMENT),
  SL_IID(DYNAMICSOURCE), SL_IID(EFFECTSEND), SL_IID(ENGINE), SL_IID(ENGINECAPABILITIES),
  SL_IID(ENVIRONMENTALREVERB), SL_IID(EQUALIZER), SL_IID(LED), SL_IID(METADATAEXTRACTION),
  SL_IID(METADATATRAVERSAL), SL_IID(MIDIMESSAGE), SL_IID(MIDIMUTESOLO), SL_IID(MIDITEMPO),
  SL_IID(MIDITIME), SL_IID(MUTESOLO), SL_IID(NULL), SL_IID(OBJECT), SL_IID(OUTPUTMIX),
  SL_IID(PITCH), SL_IID(PLAY), SL_IID(PLAYBACKRATE), SL_IID(PREFETCHSTATUS),
  SL_IID(PRESETREVERB), SL_IID(RATEPITCH), SL_IID(RECORD), SL_IID(SEEK), SL_IID(THREADSYNC),
  SL_IID(VIBRA), SL_IID(VIRTUALIZER), SL_IID(VISUALIZATION), SL_IID(VOLUME),
  #undef SL_IID
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

void update_imports(void) {
  // no runtime hook swaps needed for the GLES1 path
}


static void *cxx_new_fake(size_t size) {
  void *p = malloc(size ? size : 1);
  if (!p)
    abort();
  return p;
}

static void cxx_delete_fake(void *p) {
  free(p);
}

static int cxa_guard_acquire_fake(uint64_t *guard) {
  return (__atomic_load_n(guard, __ATOMIC_ACQUIRE) & 1) == 0;
}

static void cxa_guard_release_fake(uint64_t *guard) {
  __atomic_store_n(guard, __atomic_load_n(guard, __ATOMIC_RELAXED) | 1, __ATOMIC_RELEASE);
}

static void cxa_pure_virtual_fake(void) {
  debugPrintf("__cxa_pure_virtual called\n");
  abort();
}

static DynLibFunction ff3_extra_functions[] = {
  { "_Znwm", (uintptr_t)&cxx_new_fake },
  { "_Znam", (uintptr_t)&cxx_new_fake },
  { "_ZdlPv", (uintptr_t)&cxx_delete_fake },
  { "_ZdaPv", (uintptr_t)&cxx_delete_fake },
  { "__cxa_guard_acquire", (uintptr_t)&cxa_guard_acquire_fake },
  { "__cxa_guard_release", (uintptr_t)&cxa_guard_release_fake },
  { "__cxa_pure_virtual", (uintptr_t)&cxa_pure_virtual_fake },
  { "__strncat_chk", (uintptr_t)&__strncat_chk_fake },
  { "glGetError", (uintptr_t)&glGetError },
};

static size_t ff3_extra_numfunctions = sizeof(ff3_extra_functions) / sizeof(*ff3_extra_functions);

void ff3_resolve_imports(so_module *mod) {
  update_imports();
  so_relocate(mod);

  const size_t num = dynlib_numfunctions + ff3_extra_numfunctions;
  DynLibFunction *combined = malloc(num * sizeof(*combined));
  if (!combined)
    abort();

  memcpy(combined, dynlib_functions, dynlib_numfunctions * sizeof(*combined));
  memcpy(combined + dynlib_numfunctions, ff3_extra_functions,
      ff3_extra_numfunctions * sizeof(*combined));

  so_resolve(mod, combined, num, 1);
  free(combined);
}