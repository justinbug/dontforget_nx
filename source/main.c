/* main.c -- Chrono Trigger (cocos2d-x 3.14.1) Switch wrapper entry point
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * Loads libc++_shared.so + libchrono.so into emulated-Android memory, wires the
 * engine's imports to native shims, then drives the cocos2d-x JNI lifecycle
 * (JNI_OnLoad -> setContext/apk/assets -> nativeInit -> nativeRender loop).
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
#include "libc_shim.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "asset.h"
#include "gfx.h"
#include "opensles.h"
#include "prefs.h"
#include "movie_player.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module cpp_mod;   // libc++_shared.so
so_module game_mod;  // libchrono.so

void ct_resolve_imports(so_module *mod);

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

  // The newlib heap backs BOTH the engine's malloc and mesa/nouveau's GPU
  // texture memory (nvMap buffers come from this heap). cocos2d-x + Chrono
  // Trigger's field maps allocate hundreds of MB of textures/render targets, so
  // the newlib heap must get the bulk of memory -- only a small fixed slice is
  // reserved for the two .so load images (libc++_shared ~2MB + libchrono ~16MB).
  size_t so_reserve = (size_t)SO_HEAP_RESERVE_MB * 1024 * 1024;
  if (so_reserve > size / 2)
    so_reserve = size / 2;
  fake_heap_size = size - so_reserve;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

static void check_data(void) {
  struct stat st;
  if (stat(SO_NAME, &st) < 0)    fatal_error("Could not find\n%s.\nCheck your data files.", SO_NAME);
  if (stat(SOCPP_NAME, &st) < 0) fatal_error("Could not find\n%s.\nCheck your data files.", SOCPP_NAME);
  if (stat(ASSETS_DIR, &st) < 0) fatal_error("Could not find the\n%s/ folder.\nCheck your data files.", ASSETS_DIR);
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920; screen_height = 1080;
    } else {
      screen_width = 1280; screen_height = 720;
    }
  } else {
    screen_width = w; screen_height = h;
  }
}

// ---------------------------------------------------------------------------
// EGL / GLES2 context on the default NWindow
// ---------------------------------------------------------------------------

static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLContext s_context = EGL_NO_CONTEXT;
static EGLSurface s_surface = EGL_NO_SURFACE;

static int egl_init(void) {
  s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!s_display) { debugPrintf("egl: no display\n"); return 0; }
  eglInitialize(s_display, NULL, NULL);
  if (!eglBindAPI(EGL_OPENGL_ES_API)) { debugPrintf("egl: bindAPI failed\n"); return 0; }

  // cocos default GLContextAttrs: RGBA8888, depth24, stencil8, no MSAA
  const EGLint cfg_attr[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
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

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, ctx_attr);
  if (!s_context) { debugPrintf("egl: no context\n"); return 0; }

  eglMakeCurrent(s_display, s_surface, s_surface, s_context);
  eglSwapInterval(s_display, 1);
  return 1;
}

static void egl_deinit(void) {
  if (s_display == EGL_NO_DISPLAY) return;
  eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (s_context) eglDestroyContext(s_display, s_context);
  if (s_surface) eglDestroySurface(s_display, s_surface);
  eglTerminate(s_display);
  s_display = EGL_NO_DISPLAY;
}

// ---------------------------------------------------------------------------
// GameMaker Studio 2 engine entry points (exported by libyoyo.so)
// ---------------------------------------------------------------------------

static int  (*e_JNI_OnLoad)(void *vm, void *reserved);
static void (*e_Startup)(void *env, void *thiz, void *apkPath, void *savePath, void *storePath, int sdkVersion, int isAmazon);
static int  (*e_Process)(void *env, void *thiz, int width, int height, float accel_x, float accel_y, float accel_z, int orientation, int type, float refreshRate);
static void (*e_TouchEvent)(void *env, void *thiz, int action, int id, float x, float y);
static void (*e_KeyEvent)(void *env, void *thiz, int action, int keycode, int unicode, int source);
static void (*e_onGPDeviceAdded)(void *env, void *thiz, int device_id, void *name, void *desc, int product_id, int vendor_id, int axis_count, int hat_count, int button_mask);
static void (*e_onGPDeviceRemoved)(void *env, void *thiz, int device_id);
static void (*e_onGPKeyDown)(void *env, void *thiz, int device_id, int keycode);
static void (*e_onGPKeyUp)(void *env, void *thiz, int device_id, int keycode);
static void (*e_onGPNativeAxis)(void *env, void *thiz, int device_id, int axis, float value);
static void (*e_onGamepadChange)(void *env, void *thiz);
static void (*e_Pause)(void *env, void *thiz, int code);
static void (*e_Resume)(void *env, void *thiz, int code);
static void (*e_onGPNativeHat)(void *env, void *thiz, int device_id, int hat, float value_x, float value_y);
extern int g_gamepad_added;

extern void *g_dsMapCreate;
extern void *g_dsMapAddString;
extern void *g_dsMapAddInt;

#define RX(sym) so_try_find_addr_rx(&game_mod, sym)

static void resolve_entry_points(void) {
  debugPrintf("yoyo_nx: resolving JNI_OnLoad\n");
  e_JNI_OnLoad         = (void *)RX("JNI_OnLoad");
  debugPrintf("yoyo_nx: resolving Startup\n");
  e_Startup            = (void *)RX("Java_com_yoyogames_runner_RunnerJNILib_Startup");
  debugPrintf("yoyo_nx: resolving Process\n");
  e_Process            = (void *)RX("Java_com_yoyogames_runner_RunnerJNILib_Process");
  debugPrintf("yoyo_nx: resolving TouchEvent\n");
  e_TouchEvent         = (void *)RX("Java_com_yoyogames_runner_RunnerJNILib_TouchEvent");
  debugPrintf("yoyo_nx: resolving KeyEvent\n");
  e_KeyEvent           = (void *)RX("Java_com_yoyogames_runner_RunnerJNILib_KeyEvent");
  debugPrintf("yoyo_nx: resolving onGPDeviceAdded\n");
  e_onGPDeviceAdded    = (void *)RX("Java_com_yoyogames_runner_RunnerJNILib_onGPDeviceAdded");
  g_onGPDeviceAdded    = (void *)e_onGPDeviceAdded;
  debugPrintf("yoyo_nx: resolving onGPDeviceRemoved\n");
  e_onGPDeviceRemoved  = (void *)RX("Java_com_yoyogames_runner_RunnerJNILib_onGPDeviceRemoved");
  debugPrintf("yoyo_nx: resolving onGPKeyDown\n");
  e_onGPKeyDown        = (void *)RX("Java_com_yoyogames_runner_RunnerJNILib_onGPKeyDown");
  debugPrintf("yoyo_nx: resolving onGPKeyUp\n");
  e_onGPKeyUp          = (void *)RX("Java_com_yoyogames_runner_RunnerJNILib_onGPKeyUp");
  debugPrintf("yoyo_nx: resolving onGPNativeAxis\n");
  e_onGPNativeAxis     = (void *)RX("Java_com_yoyogames_runner_RunnerJNILib_onGPNativeAxis");
  debugPrintf("yoyo_nx: resolving onGPNativeHat\n");
  e_onGPNativeHat      = (void *)RX("Java_com_yoyogames_runner_RunnerJNILib_onGPNativeHat");
  debugPrintf("yoyo_nx: resolving onGamepadChange\n");
  e_onGamepadChange    = (void *)RX("Java_com_yoyogames_runner_RunnerJNILib_onGamepadChange");
  g_onGamepadChange    = (void *)e_onGamepadChange;

  debugPrintf("yoyo_nx: resolving GMS2 ds_map helper functions\n");
  g_dsMapCreate        = (void *)RX("Java_com_yoyogames_runner_RunnerJNILib_dsMapCreate");
  g_dsMapAddString     = (void *)RX("Java_com_yoyogames_runner_RunnerJNILib_dsMapAddString");
  g_dsMapAddInt        = (void *)RX("Java_com_yoyogames_runner_RunnerJNILib_dsMapAddInt");

  debugPrintf("yoyo_nx: resolving Pause and Resume\n");
  e_Pause              = (void *)RX("Java_com_yoyogames_runner_RunnerJNILib_Pause");
  e_Resume             = (void *)RX("Java_com_yoyogames_runner_RunnerJNILib_Resume");

  debugPrintf("yoyo_nx: all entry points resolved\n");
}

static void *thiz; // fake RunnerActivity instance handed to JNI entry points

// ---------------------------------------------------------------------------
// input -- Switch HID -> GameMaker gamepad inputs
// ---------------------------------------------------------------------------

typedef struct {
  u64 mask;
  int gp_key;
  int kb_key;
  int unicode;
} KeyMap;

static const KeyMap g_keymap[] = {
  { HidNpadButton_A,     96,  54,  90 },   // Z / 'Z' (Gamepad A)
  { HidNpadButton_B,     97,  52,  88 },   // X / 'X' (Gamepad B)
  { HidNpadButton_X,     99,  31,  67 },   // C / 'C' (Gamepad X)
  { HidNpadButton_Y,     100, 62,  32 },   // Space / ' ' (Gamepad Y)
  { HidNpadButton_L,     102, 0,   0  },   // Gamepad L1
  { HidNpadButton_R,     103, 0,   0  },   // Gamepad R1
  { HidNpadButton_ZL,    104, 0,   0  },   // Gamepad L2
  { HidNpadButton_ZR,    105, 0,   0  },   // Gamepad R2
  { HidNpadButton_Plus,  108, 66,  13 },   // Enter / '\r' (Gamepad Start)
  { HidNpadButton_Minus, 109, 111, 0  },   // Escape (Gamepad Select)
  { HidNpadButton_StickL, 106, 0,   0  },   // Gamepad Thumbl
  { HidNpadButton_StickR, 107, 0,   0  },   // Gamepad Thumbr
  { HidNpadButton_Up,    19,  19,  0  },   // KEYCODE_DPAD_UP (Gamepad Up)
  { HidNpadButton_Down,  20,  20,  0  },   // KEYCODE_DPAD_DOWN (Gamepad Down)
  { HidNpadButton_Left,  21,  21,  0  },   // KEYCODE_DPAD_LEFT (Gamepad Left)
  { HidNpadButton_Right, 22,  22,  0  },   // KEYCODE_DPAD_RIGHT (Gamepad Right)
};
#define NUM_KEYMAP (sizeof(g_keymap) / sizeof(*g_keymap))

static PadState pad;
static int g_prev[NUM_KEYMAP];

static void send_button(int idx, int pressed) {
  const KeyMap *k = &g_keymap[idx];
  int action = pressed ? 0 : 1; // 0 = ACTION_DOWN, 1 = ACTION_UP

  // 1. Send Gamepad button event (device ID 0)
  if (k->gp_key != 0) {
    if (pressed) {
      if (e_onGPKeyDown) e_onGPKeyDown(fake_env, thiz, 0, k->gp_key);
    } else {
      if (e_onGPKeyUp) e_onGPKeyUp(fake_env, thiz, 0, k->gp_key);
    }
  }

  // 2. Send Keyboard event
  if (k->kb_key != 0 && e_KeyEvent) {
    e_KeyEvent(fake_env, thiz, action, k->kb_key, k->unicode, 0x101); // 0x101 = SOURCE_KEYBOARD
  }

  // 3. Double-mapping logic for standard keyboard keys
  if (k->mask == HidNpadButton_A) {
    if (e_KeyEvent) e_KeyEvent(fake_env, thiz, action, 62, 32, 0x101); // KEYCODE_SPACE
  } else if (k->mask == HidNpadButton_B) {
    if (e_KeyEvent) e_KeyEvent(fake_env, thiz, action, 62, 32, 0x101); // KEYCODE_SPACE
  } else if (k->mask == HidNpadButton_X) {
    if (e_KeyEvent) e_KeyEvent(fake_env, thiz, action, 62, 32, 0x101); // KEYCODE_SPACE
  } else if (k->mask == HidNpadButton_Y) {
    if (e_KeyEvent) e_KeyEvent(fake_env, thiz, action, 62, 32, 0x101); // KEYCODE_SPACE
  } else if (k->mask == HidNpadButton_Plus) {
    if (e_KeyEvent) {
      e_KeyEvent(fake_env, thiz, action, 66, 10, 0x101); // KEYCODE_ENTER with '\n'
      e_KeyEvent(fake_env, thiz, action, 111, 0, 0x101); // KEYCODE_ESCAPE
    }
  }
}

static void update_keys(void) {
  padUpdate(&pad);
  u64 d = padGetButtons(&pad);

  // Read both sticks once (reuse below for axis reporting -- avoids double HID call)
  HidAnalogStickState l = padGetStickPos(&pad, 0);
  HidAnalogStickState r = padGetStickPos(&pad, 1);

  // Synthesize Left Analog Stick to D-pad buttons for 2D platformer movement
  if (l.x < -15000) d |= HidNpadButton_Left;
  if (l.x > 15000)  d |= HidNpadButton_Right;
  if (l.y < -15000) d |= HidNpadButton_Down;
  if (l.y > 15000)  d |= HidNpadButton_Up;

  if (g_gamepad_added) {
    // 1. Send Gamepad button events (only on change)
    for (unsigned i = 0; i < NUM_KEYMAP; i++) {
      const int now = (d & g_keymap[i].mask) ? 1 : 0;
      if (now != g_prev[i]) {
        send_button((int)i, now);
        g_prev[i] = now;
      }
    }

    // 2. Send Gamepad D-pad (Hat) events (only on change)
    float hat_x = 0.0f;
    float hat_y = 0.0f;
    if (d & HidNpadButton_Left)  hat_x = -1.0f;
    if (d & HidNpadButton_Right) hat_x = 1.0f;
    if (d & HidNpadButton_Up)    hat_y = -1.0f;
    if (d & HidNpadButton_Down)  hat_y = 1.0f;

    static float prev_hat_x = 0.0f;
    static float prev_hat_y = 0.0f;
    if (hat_x != prev_hat_x || hat_y != prev_hat_y) {
      if (e_onGPNativeHat)
        e_onGPNativeHat(fake_env, thiz, 0, 0, hat_x, hat_y);
      prev_hat_x = hat_x;
      prev_hat_y = hat_y;
    }

    // 3. Send Analog Stick values to Gamepad axes (only on change, with deadzone)
    if (e_onGPNativeAxis) {
      const float dz = 0.15f;
      const float axes[4] = {
        l.x / 32768.0f, -l.y / 32768.0f,
        r.x / 32768.0f, -r.y / 32768.0f
      };
      static const int axis_ids[4] = { 0, 1, 2, 3 };
      static float prev_axis[4] = { -9.0f, -9.0f, -9.0f, -9.0f };

      for (int i = 0; i < 4; i++) {
        float v = (axes[i] > -dz && axes[i] < dz) ? 0.0f : axes[i];
        if (v != prev_axis[i]) {
          e_onGPNativeAxis(fake_env, thiz, 0, axis_ids[i], v);
          prev_axis[i] = v;
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// touch -- single pointer mapped into GameMaker screen space
// ---------------------------------------------------------------------------

static int touch_down = 0;
static float last_tx = 0, last_ty = 0;

static void update_touch(void) {
  HidTouchScreenState st = {0};
  int have = hidGetTouchScreenStates(&st, 1) && st.count > 0;

  if (have) {
    const float sx = (float)screen_width / 1280.0f;
    const float sy = (float)screen_height / 720.0f;
    float x = st.touches[0].x * sx;
    float y = st.touches[0].y * sy;
    if (!touch_down) {
      touch_down = 1;
      if (e_TouchEvent) e_TouchEvent(fake_env, thiz, 0, 0, x, y); // ACTION_DOWN
    } else {
      if (e_TouchEvent) e_TouchEvent(fake_env, thiz, 2, 0, x, y); // ACTION_MOVE
    }
    last_tx = x; last_ty = y;
  } else if (touch_down) {
    touch_down = 0;
    if (e_TouchEvent) e_TouchEvent(fake_env, thiz, 1, 0, last_tx, last_ty); // ACTION_UP
  }
}

// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
  if (argc > 0 && argv[0]) {
    char *dir = strdup(argv[0]);
    if (dir) {
      char *slash = strrchr(dir, '/');
      if (slash) {
        *slash = 0;
        chdir(dir);
      }
      free(dir);
    }
  }

  debugPrintf("yoyo_nx: main started\n");
  cpu_boost(1);
  ctype_init();

  if (read_config(CONFIG_NAME) != 0)
    write_config(CONFIG_NAME);

  debugPrintf("yoyo_nx: checking syscalls\n");
  check_syscalls();
  debugPrintf("yoyo_nx: checking data files\n");
  check_data();
  set_screen_size(config.screen_width, config.screen_height);

  debugPrintf("yoyo_nx: initializing pl and gfx\n");
  plInitialize(PlServiceType_User);
  gfx_init();

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO) < 0)
    debugPrintf("SDL_Init(audio) failed: %s\n", SDL_GetError());

  debugPrintf("yoyo_nx: initializing egl\n");
  if (!egl_init())
    fatal_error("Failed to create an OpenGL ES 2.0 context.");

  // --- load both modules: libc++_shared first so libyoyo's std imports bind ---
  debugPrintf("yoyo_nx: loading libc++_shared.so\n");
  if (so_load(&cpp_mod, SOCPP_NAME, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", SOCPP_NAME);

  void *chrono_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + cpp_mod.load_size, 0x100000);
  size_t used = (uintptr_t)chrono_base - (uintptr_t)heap_so_base;
  debugPrintf("yoyo_nx: loading libyoyo.so\n");
  if (so_load(&game_mod, SO_NAME, chrono_base, heap_so_limit - used) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);

  // relocate + resolve (libyoyo's std::/__cxa_ symbols resolve into libc++_shared)
  debugPrintf("yoyo_nx: resolving imports of cpp_mod\n");
  ct_resolve_imports(&cpp_mod);
  debugPrintf("yoyo_nx: resolving imports of game_mod\n");
  ct_resolve_imports(&game_mod);

  // resolve every engine entry point now, while the symbol tables (in load_base)
  // are still readable -- so_finalize maps the code and locks load_base out.
  debugPrintf("yoyo_nx: resolving entry points\n");
  resolve_entry_points();
  if (!e_Startup || !e_Process)
    fatal_error("Could not resolve GameMaker engine entry points (Startup/Process).");
  // JNI_OnLoad is optional in GMS2 -- some builds inline it into Startup

  debugPrintf("yoyo_nx: finalizing modules\n");
  so_finalize(&cpp_mod);
  so_finalize(&game_mod);
  so_flush_caches(&cpp_mod);
  so_flush_caches(&game_mod);

  tls_setup_guard();

  // C++ static constructors: runtime first, then the game.
  debugPrintf("yoyo_nx: executing static constructors of cpp_mod\n");
  so_execute_init_array(&cpp_mod);
  debugPrintf("yoyo_nx: executing static constructors of game_mod\n");
  so_execute_init_array(&game_mod);
  so_free_temp(&cpp_mod);
  so_free_temp(&game_mod);

  // --- JNI + GameMaker bootstrap ---
  debugPrintf("yoyo_nx: bootstrapping JNI\n");
  jni_init();
  thiz = jni_make_object("RunnerActivity");

  // writable / save directory = the game directory (CWD)
  static char wdir[512];
  if (getcwd(wdir, sizeof(wdir)) && wdir[0]) {
    size_t n = strlen(wdir);
    if (n > 1 && wdir[n - 1] == '/') wdir[n - 1] = 0;
  } else {
    strcpy(wdir, ".");
  }
  jni_set_writable_path(wdir);
  {
    char prefs_path[600];
    snprintf(prefs_path, sizeof(prefs_path), "%s/YoYoPrefsFile.txt", wdir);
    prefs_init(prefs_path);
  }

  // JniHelper::setJavaVM
  if (e_JNI_OnLoad) {
    debugPrintf("yoyo_nx: calling JNI_OnLoad\n");
    e_JNI_OnLoad(fake_vm, NULL);
  }

  // Call Startup
  // apkPath: the real APK placed next to the NRO as game.apk
  // savePath / storePath: MUST end with '/' -- GMS2 runner concatenates filenames
  // directly. Also must NOT contain "sdmc:" prefix -- the runner prepends it
  // again when it stores paths internally, causing double-paths.
  debugPrintf("yoyo_nx: calling Startup\n");
  {
    char apk_path[600];
    char save_path[600];
    snprintf(apk_path, sizeof(apk_path), "%s/game.apk", wdir);
    // Strip "sdmc:" prefix if present -- use plain POSIX path
    const char *posix_dir = wdir;
    if (strncmp(posix_dir, "sdmc:", 5) == 0) posix_dir += 5;
    snprintf(save_path, sizeof(save_path), "%s/", posix_dir);
    debugPrintf("yoyo_nx: apk_path=%s save_path=%s\n", apk_path, save_path);
    e_Startup(fake_env, thiz, jni_make_string(apk_path),
              jni_make_string(save_path), jni_make_string(save_path), 28, 0);
  }

  debugPrintf("yoyo_nx: Startup returned\n");



  // input
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);
  hidInitializeTouchScreen();
  for (unsigned i = 0; i < NUM_KEYMAP; i++) g_prev[i] = 0;

  int paused = 0;
  int boot_frames = 0;
  int save_ticker = 0;
  while (appletMainLoop() && !jni_quit_requested) {
    const int focused = appletGetFocusState() == AppletFocusState_InFocus;
    if (!focused && !paused) {
      if (e_Pause) e_Pause(fake_env, thiz, 0);
      paused = 1;
      prefs_flush(); // flush saves when backgrounded (safe moment for SD write)
    }
    else if (focused && paused) {
      if (e_Resume) e_Resume(fake_env, thiz, 0);
      paused = 0;
    }

    if (paused) {
      svcSleepThread(16000000ull); // ~16ms; don't spin while backgrounded
      continue;
    }

    update_keys();
    update_touch();

    if (boot_frames == 0)
      debugPrintf("yoyo_nx: calling first Process frame\n");
    e_Process(fake_env, thiz, screen_width, screen_height, 0.0f, 0.0f, 0.0f, 0, 0, 60.0f);
    if (boot_frames == 0)
      debugPrintf("yoyo_nx: first Process frame returned\n");
    eglSwapBuffers(s_display, s_surface);

    // Keep CPU boost for first ~2 seconds (120 frames) -- GMS2 asset loading
    // finishes well within this window; dropping boost too early causes stutters.
    if (boot_frames < 120 && ++boot_frames == 120)
      cpu_boost(0);

    // Periodic save flush every ~300 frames (~5s) -- avoids per-frame SD writes
    // while ensuring saves aren't lost on unexpected power-off.
    if (++save_ticker >= 300) {
      save_ticker = 0;
      prefs_flush();
    }
  }

  prefs_flush();
  opensles_shutdown();
  egl_deinit();
  plExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
