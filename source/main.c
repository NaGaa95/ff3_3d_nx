/* main.c -- FF3 3D Switch wrapper entry point
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "obb.h"
#include "gfx.h"
#include "opensles.h"
#include "movie_player.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module game_mod;

void ff3_resolve_imports(so_module *mod);

// provide a replacement heap init so the newlib heap is separate from the .so
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_size  = umin(size, MEMORY_MB * 1024 * 1024);
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.");
}

static int has_obb_data(void) {
  struct stat st;
  return stat(OBB_NAME, &st) == 0;
}

static void check_data(void) {
  struct stat st;
  if (stat(SO_NAME, &st) < 0)
    fatal_error("Could not find\n%s.\nCheck your data files.", SO_NAME);
  if (!has_obb_data())
    fatal_error("Could not find\n%s.\nCheck your data files.", OBB_NAME);
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920;
      screen_height = 1080;
    } else {
      screen_width = 1280;
      screen_height = 720;
    }
  } else {
    screen_width = w;
    screen_height = h;
  }
}

// ---------------------------------------------------------------------------
// EGL / GLES1 context on the default NWindow
// ---------------------------------------------------------------------------

static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLContext s_context = EGL_NO_CONTEXT;
static EGLSurface s_surface = EGL_NO_SURFACE;

static int egl_init(void) {
  s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!s_display) { debugPrintf("egl: no display\n"); return 0; }
  eglInitialize(s_display, NULL, NULL);
  if (!eglBindAPI(EGL_OPENGL_ES_API)) { debugPrintf("egl: bindAPI failed\n"); return 0; }

  const EGLint cfg_attr[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT, // GLES1
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
  };
  EGLConfig config;
  EGLint num = 0;
  if (!eglChooseConfig(s_display, cfg_attr, &config, 1, &num) || num < 1) {
    debugPrintf("egl: no config\n");
    return 0;
  }

  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  s_surface = eglCreateWindowSurface(s_display, config, win, NULL);
  if (!s_surface) { debugPrintf("egl: no surface\n"); return 0; }

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE };
  s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, ctx_attr);
  if (!s_context) { debugPrintf("egl: no context\n"); return 0; }

  eglMakeCurrent(s_display, s_surface, s_surface, s_context);
  eglSwapInterval(s_display, 1);
  return 1;
}

static void egl_deinit(void) {
  if (s_display == EGL_NO_DISPLAY)
    return;
  eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (s_context) eglDestroyContext(s_display, s_context);
  if (s_surface) eglDestroySurface(s_display, s_surface);
  eglTerminate(s_display);
  s_display = EGL_NO_DISPLAY;
}

// ---------------------------------------------------------------------------
// engine entry points forwarded by libjniproxy.so
// ---------------------------------------------------------------------------

static void (*e_touch)(void *env, void *thiz, int count, int action, float x0, float y0, float x1, float y1, int keys);
static void (*e_render)(void *env, void *thiz, int width, int height, int cutout);
static void (*e_resume)(void *env, void *thiz);
static void (*e_pause)(void *env, void *thiz);

static void resolve_entry_points(void) {
  e_touch       = (void *)so_find_addr_rx(&game_mod, "touch");
  e_render      = (void *)so_find_addr_rx(&game_mod, "render");
  e_resume      = (void *)so_find_addr_rx(&game_mod, "resume");
  e_pause       = (void *)so_find_addr_rx(&game_mod, "pause");
}

static void *thiz;

static void norm_in_view(float px, float py, float *nx, float *ny) {
  *nx = (px - jni_view_x) / (float)jni_view_w;
  *ny = (py - jni_view_y) / (float)jni_view_h;
}

// The engine polls touch state every frame (onDrawFrame): it wants the live
// pointer count in both the count and action args, with 0 clearing the input
// accumulator when idle. Matches the ff3_vita / ff4_3d_nx ports.
static void update_touch(void) {
  int count = 0;
  float nx = 0, ny = 0, nx1 = 0, ny1 = 0;

  HidTouchScreenState st = {0};
  if (hidGetTouchScreenStates(&st, 1) && st.count > 0) {
    // libnx reports panel coordinates in 1280x720 space; scale to our screen
    const float sx = (float)screen_width / 1280.0f;
    const float sy = (float)screen_height / 720.0f;
    norm_in_view(st.touches[0].x * sx, st.touches[0].y * sy, &nx, &ny);
    nx1 = nx; ny1 = ny;
    if (st.count > 1)
      norm_in_view(st.touches[1].x * sx, st.touches[1].y * sy, &nx1, &ny1);
    count = st.count > 2 ? 2 : (int)st.count;
  }

  e_touch(fake_env, thiz, count, count, nx, ny, nx1, ny1, jni_get_key_mask());
}

// Android engine button bitmask bits (from MainActivity's keycode->bit table)
#define K_A     0x00001 // confirm
#define K_B     0x00002 // cancel
#define K_RIGHT 0x00010
#define K_LEFT  0x00020
#define K_UP    0x00040
#define K_DOWN  0x00080
#define K_R1    0x00100 // map
#define K_L1    0x00200
#define K_Y     0x00400 // pause menu
#define K_X     0x00800 // party interaction

static PadState pad;

// left-stick magnitude (of 32767) before it registers; ~12%, above resting drift
#define STICK_DEADZONE 4000

static void update_keys(void) {
  padUpdate(&pad);
  const u64 d = padGetButtons(&pad);
  int m = 0;
  // Nintendo-native: A (right) confirms, B (bottom) cancels.
  // Match the DS/PSP layout: X (top) opens the menu, Y (left) interacts.
  if (d & HidNpadButton_A) m |= K_A;
  if (d & HidNpadButton_B) m |= K_B;
  if (d & HidNpadButton_X) m |= K_Y;  // menu
  if (d & HidNpadButton_Y) m |= K_X;  // party interaction
  if (d & HidNpadButton_L) m |= K_L1;
  if (d & HidNpadButton_R) m |= K_R1;  // map
  if (d & HidNpadButton_Plus)  m |= K_Y;
  if (d & HidNpadButton_Minus) m |= K_R1;

  // d-pad
  if (d & HidNpadButton_Up)    m |= K_UP;
  if (d & HidNpadButton_Down)  m |= K_DOWN;
  if (d & HidNpadButton_Left)  m |= K_LEFT;
  if (d & HidNpadButton_Right) m |= K_RIGHT;
  // left stick, snapped to 8 directions: a cardinal bit holds unless the stick
  // is within ~22.5 deg of the other axis (tan 67.5 ~= 12/5). +x right, +y up.
  const HidAnalogStickState ls = padGetStickPos(&pad, 0);
  const long long ax = ls.x < 0 ? -(long long)ls.x : (long long)ls.x;
  const long long ay = ls.y < 0 ? -(long long)ls.y : (long long)ls.y;
  if (ax * ax + ay * ay > (long long)STICK_DEADZONE * STICK_DEADZONE) {
    if (12 * ax > 5 * ay) m |= (ls.x < 0 ? K_LEFT : K_RIGHT);
    if (12 * ay > 5 * ax) m |= (ls.y > 0 ? K_UP   : K_DOWN);
  }

  jni_set_keys(m);
}


int main(void) {
  cpu_boost(1);

  if (read_config(CONFIG_NAME) != 0)
    write_config(CONFIG_NAME);

  check_syscalls();
  check_data();
  set_screen_size(config.screen_width, config.screen_height);

  // shared system font for drawFont
  plInitialize(PlServiceType_User);
  gfx_init();

  // SDL is used as a library (audio only); tell it main is already running
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO) < 0)
    debugPrintf("SDL_Init(audio) failed: %s\n", SDL_GetError());

  if (!egl_init())
    fatal_error("Failed to create an OpenGL ES context.");

  if (so_load(&game_mod, SO_NAME, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);

  ff3_resolve_imports(&game_mod);

  // resolve the engine entry points now, while the symbol table (which lives in
  // load_base) is still accessible -- so_finalize maps the code via
  // svcMapProcessCodeMemory, which locks load_base out
  resolve_entry_points();
  if (!e_render || !e_resume || !e_touch)
    fatal_error("Could not resolve FF3 engine entry points.");

  so_finalize(&game_mod);
  so_flush_caches(&game_mod);

  // The engine reads its stack-protector canary from tpidr_el0+0x28.
  tls_setup_guard();

  so_execute_init_array(&game_mod); // engine C++ constructors / static init
  so_free_temp(&game_mod);

  jni_init();
  thiz = jni_make_thiz();


  if (obb_open(OBB_NAME) < 0)
    fatal_error("Could not open\n%s.\nCheck your data files.", OBB_NAME);

  jni_compute_viewport();

  e_resume(fake_env, thiz);


  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();

  int boot_frames = 0;
  int last_interval = 1;
  while (appletMainLoop() && !jni_quit_requested) {
    update_keys();
    update_touch();

    const int target = jni_get_target_fps();
    int interval = 60 / (target > 0 ? target : 60);
    if (interval < 1) interval = 1;
    if (interval != last_interval) {
      eglSwapInterval(s_display, interval);
      last_interval = interval;
    }

    e_render(fake_env, thiz, screen_width, screen_height, 0);
    movie_swap_buffers(s_display, s_surface);
    movie_main_loop_tick();

    if (boot_frames < 10 && ++boot_frames == 10)
      cpu_boost(0);
  }

  if (e_pause) e_pause(fake_env, thiz);

  opensles_shutdown();
  egl_deinit();
  obb_close();
  plExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}

