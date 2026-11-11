// rb2loader entry point: SDL2 window + GLES 1.1 context, loads the game's
// Android libraries, replays the Java activity lifecycle and turns gamepad
// input into the touch events the game expects.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include "common.h"
#include "sdl.h"

int g_verbose;
char g_gamedir[512], g_savedir[512], g_apk[512];
volatile int g_quit_requested;

static void *window;
static int scr_w, scr_h;

// ---- GL -------------------------------------------------------------------------
static void *gl_lib;
// Set when the dlopen'd libGLESv1_CM does not drive SDL's context (glGetString
// returns NULL through it): then SDL_GL_GetProcAddress is asked first.
static int gl_sdl_first;

static void *gl_resolve_real(const char *name) {
  void *p = gl_sdl_first ? pSDL_GL_GetProcAddress(name) : NULL;
  if (!p && gl_lib) p = dlsym(gl_lib, name);
  if (!p) p = pSDL_GL_GetProcAddress(name);
  if (!p) {
    // GLES 1.1 only has the framebuffer object entry points with an OES suffix.
    char alt[128];
    snprintf(alt, sizeof alt, "%sOES", name);
    if (gl_lib) p = dlsym(gl_lib, alt);
    if (!p) p = pSDL_GL_GetProcAddress(alt);
    if (p) LOG("gl: %s -> %s", name, alt);
  }
  return p;
}

void *gl_resolve(const char *name) {
  void *p = g_gles1emu ? gles1emu_proc(name) : NULL;
  return p ? p : gl_resolve_real(name);
}

static void (*glClear_)(unsigned);
static void (*glClearColor_)(float, float, float, float);
static void (*glEnable_)(unsigned), (*glDisable_)(unsigned);
static unsigned char (*glIsEnabled_)(unsigned);
static void (*glScissor_)(int, int, int, int);
static void (*glGetIntegerv_)(unsigned, int *);
static void (*glGetFloatv_)(unsigned, float *);
static void (*glGetBooleanv_)(unsigned, unsigned char *);
static void (*glColorMask_)(unsigned char, unsigned char, unsigned char, unsigned char);
static void (*glReadPixels_)(int, int, int, int, unsigned, unsigned, void *);
static const char *(*glGetString_)(unsigned);
static unsigned (*glGetError_)(void);
#define GL_SCISSOR_TEST 0x0C11
#define GL_SCISSOR_BOX 0x0C10
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100

static void gl_load_own(void) {
  glClear_ = gl_resolve("glClear");
  glClearColor_ = gl_resolve("glClearColor");
  glEnable_ = gl_resolve("glEnable");
  glDisable_ = gl_resolve("glDisable");
  glIsEnabled_ = gl_resolve("glIsEnabled");
  glScissor_ = gl_resolve("glScissor");
  glGetIntegerv_ = gl_resolve("glGetIntegerv");
  glGetFloatv_ = gl_resolve("glGetFloatv");
  glGetBooleanv_ = gl_resolve("glGetBooleanv");
  glColorMask_ = gl_resolve("glColorMask");
  glReadPixels_ = gl_resolve("glReadPixels");
  glGetString_ = gl_resolve("glGetString");
  glGetError_ = gl_resolve("glGetError");
}

// ---- symbol resolution for the Android libraries --------------------------------
static void *resolve(const char *name) {
  void *p = bionic_resolve(name);
  if (p) return p;
  if ((p = so_find_global(name))) return p;
  if (!strncmp(name, "gl", 2) || !strncmp(name, "egl", 3)) {
    if ((p = gl_resolve(name))) return p;
    LOG("gl: %s not available", name);
    return NULL;
  }
  return dlsym(RTLD_DEFAULT, name);
}

static so_module *load_lib(const char *file) {
  char path[256];
  snprintf(path, sizeof path, "lib/arm64-v8a/%s", file);
  size_t size;
  void *elf = zip_read(path, &size);
  if (!elf) { LOG("%s missing from the apk (arm64-v8a build needed)", path); return NULL; }
  so_module *m = so_load(file, elf, size, resolve);
  free(elf);
  return m;
}

// ---- crash reporting -----------------------------------------------------------
// Crash and hang reports go to stderr (log.txt) and to crash.txt, which is
// fsync'd so it survives even if the device has to be powered off.
static int crash_fd = -1;
static pid_t main_tid;

static void out(const char *fmt, ...) {
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n >= (int)sizeof b) n = sizeof b - 1;
  if (write(2, b, n) < 0) {}
  if (crash_fd < 0) crash_fd = open("crash.txt", O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
  if (crash_fd >= 0 && write(crash_fd, b, n) < 0) {}
}

static void describe(const char *what, uintptr_t addr) {
  const char *mn;
  uintptr_t off;
  Dl_info di;
  if (so_addr_info(addr, &mn, &off))
    out("[rb2]   %s %s+%#lx\n", what, mn, (unsigned long)off);
  else if (dladdr((void *)addr, &di) && di.dli_fname)
    out("[rb2]   %s %s+%#lx (%s+%#lx)\n", what, di.dli_fname,
        (unsigned long)(addr - (uintptr_t)di.dli_fbase), di.dli_sname ? di.dli_sname : "?",
        (unsigned long)(di.dli_saddr ? addr - (uintptr_t)di.dli_saddr : 0));
  else
    out("[rb2]   %s %#lx\n", what, (unsigned long)addr);
}

static void backtrace_from(mcontext_t *mc) {
  describe("pc", mc->pc);
  describe("lr", mc->regs[30]);
  // Frame-pointer chain (both the game and glibc keep x29 frames).
  uintptr_t fp = mc->regs[29], sp = mc->sp;
  for (int n = 0; n < 24 && fp >= sp && fp - sp < (8u << 20) && !(fp & 7); n++) {
    uintptr_t *f = (uintptr_t *)fp;
    if (!f[1]) break;
    describe("bt", f[1]);
    if (f[0] <= fp) break;
    fp = f[0];
  }
}

static void crash_handler(int sig, siginfo_t *si, void *uc_) {
  ucontext_t *uc = uc_;
  mcontext_t *mc = &uc->uc_mcontext;
  out("[rb2] fatal signal %d (code %d), fault address %p, tid %ld%s\n", sig, si->si_code, si->si_addr,
      (long)syscall(SYS_gettid), syscall(SYS_gettid) == main_tid ? " (main)" : "");
  // code <= 0: the signal was sent (kill/tgkill/raise), not caused by a fault.
  if (si->si_code <= 0)
    out("[rb2]   sent by pid %d uid %d (rb2loader is pid %d)\n", (int)si->si_pid, (int)si->si_uid,
        (int)getpid());
  backtrace_from(mc);
  for (int i = 0; i < 31; i += 4)
    out("[rb2]   x%-2d %016llx %016llx %016llx %016llx\n", i, (unsigned long long)mc->regs[i],
        (unsigned long long)mc->regs[i + 1], (unsigned long long)(i + 2 < 31 ? mc->regs[i + 2] : mc->sp),
        (unsigned long long)(i + 3 < 31 ? mc->regs[i + 3] : 0));
  if (crash_fd >= 0) fsync(crash_fd);
  _exit(128 + sig);
}

// SIGUSR2 from the watchdog: print where this thread is and carry on.
static void where_handler(int sig, siginfo_t *si, void *uc_) {
  (void)sig; (void)si;
  ucontext_t *uc = uc_;
  long tid = syscall(SYS_gettid);
  out("[rb2] thread %ld%s:\n", tid, tid == main_tid ? " (main)" : "");
  backtrace_from(&uc->uc_mcontext);
}

// Signals sent from outside (hangup, terminate...): log who sent them. Hangup
// and broken pipe are ignored so the game keeps running; the others quit cleanly.
static void ext_handler(int sig, siginfo_t *si, void *uc_) {
  (void)uc_;
  char comm[64] = "?", path[32];
  snprintf(path, sizeof path, "/proc/%d/comm", (int)si->si_pid);
  int fd = open(path, O_RDONLY);
  if (fd >= 0) {
    ssize_t n = read(fd, comm, sizeof comm - 1);
    comm[n > 0 ? n : 0] = 0;
    if (n > 0 && comm[n - 1] == '\n') comm[n - 1] = 0;
    close(fd);
  }
  out("[rb2] got signal %d (code %d) from pid %d (%s) uid %d\n", sig, si->si_code, (int)si->si_pid, comm,
      (int)si->si_uid);
  if (crash_fd >= 0) fsync(crash_fd);
  if (sig == SIGHUP || sig == SIGPIPE) return;
  if (g_quit_requested) _exit(128 + sig);
  g_quit_requested = 1;
}

static void install_crash_handler(void) {
  struct sigaction ea;
  memset(&ea, 0, sizeof ea);
  ea.sa_sigaction = ext_handler;
  ea.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
  static const int ext[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGPIPE, SIGUSR1, SIGALRM, SIGXCPU,
                             SIGXFSZ, SIGVTALRM, SIGPROF, SIGSYS, SIGTRAP, SIGSTKFLT, SIGPWR };
  for (size_t i = 0; i < sizeof ext / sizeof *ext; i++) sigaction(ext[i], &ea, NULL);

  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
  sa.sa_sigaction = where_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
  sigaction(SIGUSR2, &sa, NULL);
}

// ---- hang watchdog -------------------------------------------------------------
// If the main loop stops turning, dump every thread's position into the log and,
// if it never recovers, quit so the device is not left on a frozen screen.
static volatile unsigned heartbeat;

static long rss_mb(void) {
  long pages = 0, rss = 0;
  FILE *f = fopen("/proc/self/statm", "r");
  if (f) {
    if (fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0;
    fclose(f);
  }
  return rss * (sysconf(_SC_PAGESIZE) / 1024) / 1024;
}

// MemAvailable from /proc/meminfo, in MB: watches for system-wide memory pressure.
static long avail_mb(void) {
  long kb = -1;
  char line[128];
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f) return -1;
  while (fgets(line, sizeof line, f))
    if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) break;
  fclose(f);
  return kb < 0 ? -1 : kb / 1024;
}

static void dump_threads(void) {
  int tids[64];
  int n = bionic_thread_tids(tids, 64);
  syscall(SYS_tgkill, getpid(), main_tid, SIGUSR2);
  usleep(300000);
  for (int i = 0; i < n; i++) {
    if (syscall(SYS_tgkill, getpid(), tids[i], SIGUSR2) == 0) usleep(300000);
  }
  if (crash_fd >= 0) fsync(crash_fd);
}

static void *watchdog(void *arg) {
  (void)arg;
  unsigned last = heartbeat;
  int stalled = 0;
  for (;;) {
    sleep(1);
    fdatasync(2); // log.txt is a plain file: keep it on the card in case of a freeze
    if (heartbeat != last) {
      if (stalled >= 8) LOG("watchdog: main loop running again after %d s", stalled);
      last = heartbeat;
      stalled = 0;
      continue;
    }
    stalled++;
    if (stalled == 8 || stalled == 30) {
      out("[rb2] watchdog: main loop stuck for %d s (RSS %ld MB), thread positions:\n", stalled, rss_mb());
      dump_threads();
    }
    if (stalled == 60) {
      out("[rb2] watchdog: giving up, quitting\n");
      if (crash_fd >= 0) fsync(crash_fd);
      rb2_quit(3);
    }
  }
  return NULL;
}

// ---- the Java entry points of librobberybob ------------------------------------
#define JN(x) "Java_com_chillingo_robberybob2_android_gplay_JavaNative_" #x
static void *env;
static jobj *thiz;
static void (*j_InitJava)(void *, jobj *);
static void (*j_SetVersionStrings)(void *, jobj *, jobj *, jobj *);
static void (*j_RegisterJavaMethod)(void *, jobj *, jobj *, jobj *, jobj *, jobj *, int);
static void (*j_InitGame)(void *, jobj *, int, int);
static void (*j_SetScreen)(void *, jobj *, int, int);
static void (*j_UpdateAndRender)(void *, jobj *);
static void (*j_ReportTouches)(void *, jobj *, int, jobj *, jobj *, int, double);
static void (*j_BackButton)(void *, jobj *);
static void (*j_BecomeActive)(void *, jobj *);
static void (*j_ResignActive)(void *, jobj *);

static void register_java(const char *cls, const char *const *methods) {
  jobj *o = jni_new_object(cls);
  jobj *c = jni_new_string(cls);
  for (; *methods; methods += 2)
    j_RegisterJavaMethod(env, thiz, c, jni_new_string(methods[0]), jni_new_string(methods[1]), o, 0);
}

static void register_all(void) {
  static const char *const activity[] = {
    "QuitGame", "()V", "OnQuitGameComplete", "()V", "GameInitComplete", "()V",
    "RemoveSplashes", "()V", "ShowOffers", "()V", "CloseOffers", "()V",
    "IsAgeVerificationCriteriaMet", "()Z", "UseOBB", "()Z", "OpenURL", "(Ljava/lang/String;)V",
    "GetDeviceLocale", "()Ljava/lang/String;", "GetLocaleCountryCode", "()Ljava/lang/String;",
    "URLRequest", "(Ljava/lang/String;)V", "SC", "(Z)V", "isLifesFeatureEnabled", "()Z", NULL };
  static const char *const services[] = {
    "SignIn", "()V", "IsSignedIn", "()Z", "SignOut", "()V", "ShowLeaderboard", "(Ljava/lang/String;)V",
    "ShowAchievements", "()V", "UnlockAchievement", "(Ljava/lang/String;)V",
    "IncrementAchievement", "(Ljava/lang/String;I)V", "PostScore", "(Ljava/lang/String;I)V",
    "ShowSavedGamesUI", "()V", NULL };
  static const char *const renderer[] = { "TakeScreenShot", "()V", NULL };
  static const char *const data[] = {
    "internalSave", "(Ljava/lang/String;[B)V", "internalLoad", "(Ljava/lang/String;)[B", NULL };
  static const char *const firebase[] = {
    "GetConfigValueForKey", "(Ljava/lang/String;)Ljava/lang/String;",
    "AddKeyAndValueString", "(Ljava/lang/String;Ljava/lang/String;)V",
    "AddKeyAndValueInt", "(Ljava/lang/String;I)V", "AddKeyAndValueFloat", "(Ljava/lang/String;F)V",
    "AddKeyAndValueDouble", "(Ljava/lang/String;D)V", "AddKeyAndValueBoolean", "(Ljava/lang/String;Z)V",
    "LogEventWithName", "(Ljava/lang/String;Z)V", "ClearParameterCache", "()V", NULL };
  static const char *const analytics[] = {
    "AddResourceEventWithFlowType", "(ZLjava/lang/String;FLjava/lang/String;Ljava/lang/String;)V",
    "AddProgressionEventWithProgressionStatus", "(Ljava/lang/String;FLjava/lang/String;Ljava/lang/String;)V", NULL };
  static const char *const sound[] = {
    "loadMusic", "(Ljava/lang/String;)Z", "playMusic", "(Z)V", "stopMusic", "()V",
    "isPlayingMusic", "()Z", "unloadSound", "(Ljava/lang/String;I)V",
    "loadSoundEffect", "(Ljava/lang/String;)I", "playSoundEffect", "(II)I",
    "setSoundVolume", "(IF)V", "setRate", "(IF)V", "playSoundEffectWithVolume", "(IIF)I",
    "stopSound", "(I)V", "isSoundEffectPlaying", "(I)Z", "setMusicVolume", "(F)V",
    "setSoundVolume", "(F)V", "getMusicCurrentTime", "()F", "setMusicCurrentTime", "(F)V",
    "getSoundDuration", "(Ljava/lang/String;)F", NULL };
  static const char *const youtube[] = {
    "StopYoutubePlayer", "()Z", "ResumeYoutubePlayer", "()Z",
    "StartYoutubePlaylistWithID", "(Ljava/lang/String;)Z", "StartVideoWithID", "(Ljava/lang/String;)Z", NULL };
  static const char *const billing[] = {
    "UpdateProductList", "([Ljava/lang/String;)V", "RequestPurchase", "(Ljava/lang/String;)V", NULL };
  static const char *const ironsrc[] = {
    "ShowAdWithZoneID", "(Ljava/lang/String;)V", "ShowRewardAdWithZoneID", "(Ljava/lang/String;)V",
    "IsRewardedVideoAvailable", "()Z", "IsInterstitialAvailable", "()Z", "GetOfferwallCredits", "()V",
    "PreloadInterstitial", "()V", "IsRewardedVideoLoaded", "()V", "IsOfferwallAvailable", "()Z",
    "ShowOfferwall", "(Ljava/lang/String;)V", "MakeSureLoaderThreadIsRunning", "()V", NULL };
  // Same order as GameActivity.e() on Android.
  register_java("com/chillingo/robberybob2/android/gplay/Renderer", renderer);
  register_java("com/chillingo/robberybob2/android/gplay/GameActivity", activity);
  register_java("se/leveleight/utils/leFirebase", firebase);
  register_java("se/leveleight/utils/leGameAnalytics", analytics);
  register_java("se/leveleight/utils/leIronSrc", ironsrc);
  register_java("se/leveleight/utils/leGameSpecificData", data);
  register_java("se/leveleight/utils/leBillingv2", billing);
  register_java("se/leveleight/utils/leSoundManager", sound);
  register_java("se/leveleight/utils/leYoutubePlayer", youtube);
  register_java("com/chillingo/robberybob2/android/gplay/GameServiceManager", services);
}

// ---- touch emulation -------------------------------------------------------------
// Mirrors leGLSurfaceView.onTouchEvent: a[] holds the current x,y pairs, b[] the
// previous ones, c the pointer count (at most 5); on a pointer going up the
// count already excludes it.
enum { SLOT_JOY, SLOT_CURSOR, SLOT_X, SLOT_Y, SLOT_L1, SLOT_R1, SLOT_L2, SLOT_R2, NSLOTS };
typedef struct { int active; float x, y; int want; float wx, wy; } touch;
static touch touches[NSLOTS];
static jobj *arr_a, *arr_b;
static int t_count;

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void report(void) {
  j_ReportTouches(env, thiz, t_count, arr_a, arr_b, 16, now_ms());
}

static int active_count(void) {
  int n = 0;
  for (int i = 0; i < NSLOTS; i++) n += touches[i].active;
  return n;
}
static int rank(int slot) {
  int r = 0;
  for (int i = 0; i < slot; i++) r += touches[i].active;
  return r;
}

static void touch_down(int slot, float x, float y) {
  int *a = jni_int_data(arr_a), *b = jni_int_data(arr_b);
  touches[slot].active = 1;
  touches[slot].x = x;
  touches[slot].y = y;
  int n = active_count();
  t_count = n > 5 ? 5 : n;
  int i = rank(slot);
  if (i < t_count) {
    a[2 * i] = b[2 * i] = (int)x;
    a[2 * i + 1] = b[2 * i + 1] = (int)y;
  }
  VLOG("touch down %d at %d,%d", slot, (int)x, (int)y);
  report();
}

static void touch_move(void) {
  int *a = jni_int_data(arr_a), *b = jni_int_data(arr_b);
  int n = active_count();
  t_count = n > 5 ? 5 : n;
  for (int s = 0, i = 0; s < NSLOTS && i < t_count; s++) {
    if (!touches[s].active) continue;
    b[2 * i] = a[2 * i];
    b[2 * i + 1] = a[2 * i + 1];
    a[2 * i] = (int)touches[s].x;
    a[2 * i + 1] = (int)touches[s].y;
    i++;
  }
  report();
}

static void touch_up(int slot) {
  int *a = jni_int_data(arr_a), *b = jni_int_data(arr_b);
  int n = active_count();
  t_count = n > 5 ? 5 : n;
  int i = rank(slot);
  if (i < t_count) {
    b[2 * i] = a[2 * i];
    b[2 * i + 1] = a[2 * i + 1];
    a[2 * i] = (int)touches[slot].x;
    a[2 * i + 1] = (int)touches[slot].y;
    t_count--;
  }
  touches[slot].active = 0;
  VLOG("touch up %d", slot);
  report();
}

static void touch_sync(void) {
  for (int s = 0; s < NSLOTS; s++)
    if (touches[s].active && !touches[s].want) touch_up(s);
  int moved = 0;
  for (int s = 0; s < NSLOTS; s++) {
    touch *t = &touches[s];
    if (!t->want) continue;
    if (!t->active) { touch_down(s, t->wx, t->wy); continue; }
    if ((int)t->x != (int)t->wx || (int)t->y != (int)t->wy) { t->x = t->wx; t->y = t->wy; moved = 1; }
  }
  if (moved) touch_move();
}

// ---- gamepad ---------------------------------------------------------------------
static int16_t axes[SDL_CONTROLLER_AXIS_MAX];
static uint8_t buttons[SDL_CONTROLLER_BUTTON_MAX];
static float cur_x, cur_y;
static double cursor_seen;
static int dpad_cursor;      // Select toggles the d-pad between joystick and cursor
static int select_combo;     // Select was used as a modifier while held
static float joy_ax, joy_ay, joy_r, tap_pos[NSLOTS][2];

static void parse_point(const char *env_name, float *out, float dx, float dy) {
  const char *v = getenv(env_name);
  out[0] = dx;
  out[1] = dy;
  if (v && *v) sscanf(v, "%f,%f", &out[0], &out[1]);
}

static void input_config(void) {
  float p[2];
  parse_point("RB2_JOY", p, 0.16f, 0.74f);
  joy_ax = p[0] * scr_w;
  joy_ay = p[1] * scr_h;
  const char *r = getenv("RB2_JOY_RADIUS");
  joy_r = (r && *r ? atof(r) : 0.12f) * scr_h;
  static const char *names[NSLOTS] = { NULL, NULL, "RB2_TAP_X", "RB2_TAP_Y", "RB2_TAP_L1",
                                       "RB2_TAP_R1", "RB2_TAP_L2", "RB2_TAP_R2" };
  for (int s = SLOT_X; s < NSLOTS; s++) {
    parse_point(names[s], tap_pos[s], -1, -1);
    if (tap_pos[s][0] >= 0) LOG("%s at %.2f,%.2f", names[s], tap_pos[s][0], tap_pos[s][1]);
  }
  cur_x = scr_w / 2.0f;
  cur_y = scr_h / 2.0f;
  LOG("joystick anchor %.0f,%.0f radius %.0f", joy_ax, joy_ay, joy_r);
}

static float axis(int a) {
  float v = axes[a] / 32767.0f;
  return fabsf(v) < 0.2f ? 0 : v;
}

static void screenshot(void);

static void on_button(int b, int down) {
  buttons[b] = down;
  if (b == SDL_CONTROLLER_BUTTON_BACK) {
    if (down) select_combo = 0;
    else if (!select_combo) {
      dpad_cursor = !dpad_cursor;
      LOG("d-pad now moves the %s", dpad_cursor ? "cursor" : "joystick");
      cursor_seen = now_ms();
    }
    return;
  }
  if (!down) return;
  if (buttons[SDL_CONTROLLER_BUTTON_BACK]) {
    select_combo = 1;
    if (b == SDL_CONTROLLER_BUTTON_START) g_quit_requested = 1;
    else if (b == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) screenshot();
    return;
  }
  if (b == SDL_CONTROLLER_BUTTON_B || b == SDL_CONTROLLER_BUTTON_START) {
    if (j_BackButton) j_BackButton(env, thiz);
  }
}

static void update_input(float dt) {
  // Joystick slot: left stick, or d-pad in joystick mode.
  float jx = axis(SDL_CONTROLLER_AXIS_LEFTX), jy = axis(SDL_CONTROLLER_AXIS_LEFTY);
  float cx = axis(SDL_CONTROLLER_AXIS_RIGHTX), cy = axis(SDL_CONTROLLER_AXIS_RIGHTY);
  float dx = buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] - buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT];
  float dy = buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN] - buttons[SDL_CONTROLLER_BUTTON_DPAD_UP];
  if (buttons[SDL_CONTROLLER_BUTTON_BACK]) dx = dy = 0;
  if (dpad_cursor) { cx += dx; cy += dy; cx += jx; cy += jy; jx = jy = 0; }
  else if (dx || dy) {
    float l = sqrtf(dx * dx + dy * dy);
    jx = dx / l;
    jy = dy / l;
  }
  float jl = sqrtf(jx * jx + jy * jy);
  if (jl > 1) { jx /= jl; jy /= jl; }
  touch *j = &touches[SLOT_JOY];
  j->want = jl > 0;
  if (j->want) {
    if (!j->active) { j->wx = joy_ax; j->wy = joy_ay; } // land on the anchor first
    else { j->wx = joy_ax + jx * joy_r; j->wy = joy_ay + jy * joy_r; }
  }

  // Cursor: right stick (plus d-pad/left stick in cursor mode); A taps there.
  float cl = sqrtf(cx * cx + cy * cy);
  if (cl > 0) {
    if (cl > 1) { cx /= cl; cy /= cl; cl = 1; }
    float speed = scr_h * (0.35f + 1.1f * cl * cl);
    cur_x += cx * speed * dt;
    cur_y += cy * speed * dt;
    if (cur_x < 0) cur_x = 0;
    if (cur_y < 0) cur_y = 0;
    if (cur_x > scr_w - 1) cur_x = scr_w - 1;
    if (cur_y > scr_h - 1) cur_y = scr_h - 1;
    cursor_seen = now_ms();
  }
  touch *c = &touches[SLOT_CURSOR];
  c->want = buttons[SDL_CONTROLLER_BUTTON_A];
  c->wx = cur_x;
  c->wy = cur_y;
  if (c->want) cursor_seen = now_ms();

  static const int map[NSLOTS] = { -1, -1, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
                                   SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, -2, -3 };
  for (int s = SLOT_X; s < NSLOTS; s++) {
    if (tap_pos[s][0] < 0) continue;
    int held;
    if (map[s] == -2) held = axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] > 16000;
    else if (map[s] == -3) held = axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] > 16000;
    else held = buttons[map[s]] && !buttons[SDL_CONTROLLER_BUTTON_BACK];
    touches[s].want = held;
    touches[s].wx = tap_pos[s][0] * scr_w;
    touches[s].wy = tap_pos[s][1] * scr_h;
  }
  touch_sync();
}

// ---- overlay: cursor drawn with scissored clears (leaves GL state intact) -------
static void fill(int x, int y, int w, int h, float r, float g, float b) {
  glScissor_(x, scr_h - y - h, w, h);
  glClearColor_(r, g, b, 1);
  glClear_(GL_COLOR_BUFFER_BIT);
}

static void draw_cursor(void) {
  if (now_ms() - cursor_seen > 2500 || !glScissor_ || !glIsEnabled_ || !glGetFloatv_ ||
      !glGetIntegerv_ || !glGetBooleanv_ || !glColorMask_ || !glClearColor_)
    return;
  float cc[4];
  int box[4];
  unsigned char mask[4], sc = glIsEnabled_(GL_SCISSOR_TEST);
  glGetFloatv_(GL_COLOR_CLEAR_VALUE, cc);
  glGetIntegerv_(GL_SCISSOR_BOX, box);
  glGetBooleanv_(GL_COLOR_WRITEMASK, mask);
  glColorMask_(1, 1, 1, 1);
  glEnable_(GL_SCISSOR_TEST);
  int x = (int)cur_x, y = (int)cur_y, s = scr_h / 48 + 4, t = scr_h / 240 + 1;
  float v = buttons[SDL_CONTROLLER_BUTTON_A] ? 0.2f : 1.0f;
  fill(x - s - 1, y - t - 1, 2 * s + 3, 2 * t + 3, 0, 0, 0);
  fill(x - t - 1, y - s - 1, 2 * t + 3, 2 * s + 3, 0, 0, 0);
  fill(x - s, y - t, 2 * s + 1, 2 * t + 1, 1, 1, v);
  fill(x - t, y - s, 2 * t + 1, 2 * s + 1, 1, 1, v);
  if (!sc) glDisable_(GL_SCISSOR_TEST);
  glScissor_(box[0], box[1], box[2], box[3]);
  glClearColor_(cc[0], cc[1], cc[2], cc[3]);
  glColorMask_(mask[0], mask[1], mask[2], mask[3]);
}

static int want_shot;
static void screenshot(void) { want_shot = 1; }

static void take_screenshot(void) {
  want_shot = 0;
  unsigned char *px = malloc((size_t)scr_w * scr_h * 4);
  glReadPixels_(0, 0, scr_w, scr_h, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, px);
  char path[600];
  mkdir("screenshots", 0755);
  snprintf(path, sizeof path, "screenshots/rb2-%ld.bmp", (long)time(NULL));
  FILE *f = fopen(path, "wb");
  if (f) {
    int row = (scr_w * 3 + 3) & ~3;
    uint32_t size = 54 + row * scr_h;
    unsigned char h[54] = { 'B', 'M' };
    memcpy(h + 2, &size, 4);
    h[10] = 54; h[14] = 40;
    memcpy(h + 18, &scr_w, 4);
    memcpy(h + 22, &scr_h, 4);
    h[26] = 1; h[28] = 24;
    fwrite(h, 1, 54, f);
    unsigned char *line = calloc(1, row);
    for (int y = 0; y < scr_h; y++) {
      for (int x = 0; x < scr_w; x++) {
        unsigned char *p = px + ((size_t)y * scr_w + x) * 4;
        line[x * 3] = p[2]; line[x * 3 + 1] = p[1]; line[x * 3 + 2] = p[0];
      }
      fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    LOG("screenshot saved to %s", path);
  }
  free(px);
}

// ---- window and GL context ------------------------------------------------------
// Opens the fullscreen window with a GLES <major> context. Returns 1 when GL
// calls really reach that context (glGetString answers); otherwise the window
// is torn down again, except for the last attempt so the game can still run.
static void *gl_ctx;

static int create_gl(int major) {
  if (window) {
    if (gl_ctx) pSDL_GL_DeleteContext(gl_ctx);
    pSDL_DestroyWindow(window);
    window = gl_ctx = NULL;
  }
  if (gl_lib) { dlclose(gl_lib); gl_lib = NULL; }
  gl_sdl_first = 0;
  pSDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  pSDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
  pSDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, major == 1 ? 1 : 0);
  pSDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
  pSDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_DisplayMode mode;
  int w = 640, h = 480;
  if (pSDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.w > 0) { w = mode.w; h = mode.h; }
  window = pSDL_CreateWindow("Robbery Bob 2", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
  if (!window) { LOG("SDL_CreateWindow failed: %s", pSDL_GetError()); return 0; }
  gl_ctx = pSDL_GL_CreateContext(window);
  if (!gl_ctx) { LOG("GLES %d context failed: %s", major, pSDL_GetError()); return 0; }
  if (pSDL_GL_MakeCurrent(window, gl_ctx) < 0) LOG("SDL_GL_MakeCurrent failed: %s", pSDL_GetError());
  pSDL_GL_SetSwapInterval(1);
  pSDL_ShowCursor(0);
  pSDL_GL_GetDrawableSize(window, &scr_w, &scr_h);
  if (scr_w <= 0) pSDL_GetWindowSize(window, &scr_w, &scr_h);
  static const char *const es1[] = { "libGLESv1_CM.so.1", "libGLESv1_CM.so", "libGLES_CM.so.1",
                                     "libGLES_CM.so", NULL };
  static const char *const es2[] = { "libGLESv2.so.2", "libGLESv2.so", NULL };
  const char *const *names = major == 1 ? es1 : es2;
  for (int i = 0; names[i] && !gl_lib; i++) gl_lib = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
  gl_load_own();
  if (!glGetString_ || !glGetString_(0x1F02)) {
    LOG("gl: %s does not see the SDL context, trying SDL_GL_GetProcAddress", gl_lib ? names[0] : "dlsym");
    gl_sdl_first = 1;
    gl_load_own();
  }
  const char *v = glGetString_ ? glGetString_(0x1F02) : NULL;
  LOG("GLES %d context: %s", major, v ? v : "not working");
  return v != NULL;
}

// ---- lifecycle -------------------------------------------------------------------
void rb2_quit(int code) {
  fflush(stdout);
  fflush(stderr);
  _exit(code);
}

static void *must(so_module *m, const char *name) {
  void *p = so_sym(m, name);
  if (!p) LOG("missing export %s", name);
  return p;
}

int main(int argc, char **argv) {
  setvbuf(stderr, NULL, _IOLBF, 0);
  setvbuf(stdout, NULL, _IOLBF, 0);
  const char *v = getenv("RB2_VERBOSE");
  g_verbose = v && *v && *v != '0';
  if (!getcwd(g_gamedir, sizeof g_gamedir)) strcpy(g_gamedir, ".");
  if (argc < 2) { LOG("usage: %s <robberybob2.apk>", argv[0]); return 1; }
  if (!realpath(argv[1], g_apk)) snprintf(g_apk, sizeof g_apk, "%s", argv[1]);
  const char *sd = getenv("RB2_SAVE_DIR");
  snprintf(g_savedir, sizeof g_savedir, "%s", sd && *sd ? sd : "savedata");
  main_tid = (pid_t)syscall(SYS_gettid);
  LOG("rb2loader 1.9 | pid %d | apk %s | saves %s", (int)getpid(), g_apk, g_savedir);
  unlink("crash.txt");

  rb2_altstack();
  install_crash_handler();
  bionic_init();
  if (sdl_load()) return 1;
  if (zip_open(g_apk)) return 1;

  if (pSDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) < 0) {
    LOG("SDL_Init failed: %s", pSDL_GetError());
    return 1;
  }
  if (pSDL_Init(SDL_INIT_AUDIO) < 0) LOG("SDL audio init failed: %s", pSDL_GetError());
  LOG("SDL video driver: %s", pSDL_GetCurrentVideoDriver() ? pSDL_GetCurrentVideoDriver() : "?");
  // GLES 1.1 first; many handheld drivers only do GLES 2, then the fixed
  // function pipeline is emulated (gles1emu.c). RB2_GLES=1 or 2 forces one.
  const char *ge = getenv("RB2_GLES");
  int want = ge && *ge ? atoi(ge) : 0;
  int ok = 0;
  if (want != 2) ok = create_gl(1);
  if (!ok && want != 1) {
    if (want != 2) LOG("GLES 1.1 not usable, trying GLES 2.0 with fixed-function emulation");
    ok = create_gl(2);
    if (ok && gles1emu_init(gl_resolve_real)) ok = 0;
  }
  if (!window) return 1;
  const char *glv = glGetString_ ? glGetString_(0x1F02) : NULL;
  const char *glr = glGetString_ ? glGetString_(0x1F01) : NULL;
  LOG("screen %dx%d | GL %s | %s", scr_w, scr_h, glv ? glv : "(null)", glr ? glr : "(null)");
  if (!ok) LOG("WARNING: no working GLES context, the screen will stay black");

  audio_init();
  // SDL (and some GL drivers) install their own fault handlers that re-raise the
  // signal and lose the faulting context: take them back.
  install_crash_handler();
  for (int i = 0; i < pSDL_NumJoysticks(); i++)
    if (pSDL_IsGameController(i)) {
      void *gc = pSDL_GameControllerOpen(i);
      LOG("controller %d: %s", i, gc ? pSDL_GameControllerName(gc) : "open failed");
    }

  so_module *cxx = load_lib("libc++_shared.so");
  so_module *game = load_lib("librobberybob.so");
  if (!cxx || !game) return 1;
  so_run_init(cxx);
  so_run_init(game);
  LOG("init arrays done");
  pthread_t wd;
  if (pthread_create(&wd, NULL, watchdog, NULL) == 0) pthread_detach(wd);

  jni_init();
  env = jni_env();
  thiz = jni_new_object("com/chillingo/robberybob2/android/gplay/JavaNative");
  j_InitJava = must(game, JN(InitJava));
  j_SetVersionStrings = must(game, JN(SetVersionStrings));
  j_RegisterJavaMethod = must(game, JN(RegisterJavaMethod));
  j_InitGame = must(game, JN(InitGame));
  j_SetScreen = must(game, JN(SetScreenWidthAndHeight));
  j_UpdateAndRender = must(game, JN(UpdateAndRender));
  j_ReportTouches = must(game, JN(ReportTouches));
  j_BackButton = must(game, JN(BackButtonPressed));
  j_BecomeActive = must(game, JN(OnApplicationDidBecomeActive));
  j_ResignActive = must(game, JN(OnApplicationWillResignActive));
  if (!j_InitJava || !j_RegisterJavaMethod || !j_InitGame || !j_UpdateAndRender || !j_ReportTouches) return 1;

  arr_a = jni_new_int_array(16);
  arr_b = jni_new_int_array(16);
  input_config();

  LOG("InitJava");
  j_InitJava(env, thiz);
  j_SetVersionStrings(env, thiz, jni_new_string("1.8.0.2"), jni_new_string("416902"));
  register_all();
  LOG("InitGame %dx%d", scr_w, scr_h);
  j_InitGame(env, thiz, scr_w, scr_h);
  j_SetScreen(env, thiz, scr_w, scr_h);
  if (j_BecomeActive) j_BecomeActive(env, thiz);
  LOG("entering main loop");

  double last = now_ms(), fps_t = last;
  int frames = 0;
  while (!g_quit_requested) {
    SDL_Event ev;
    while (pSDL_PollEvent(&ev)) {
      switch (ev.type) {
      case SDL_QUIT: g_quit_requested = 1; break;
      case SDL_CONTROLLERDEVICEADDED: {
        void *gc = pSDL_GameControllerOpen(ev.cdevice.which);
        LOG("controller added: %s", gc ? pSDL_GameControllerName(gc) : "?");
        break;
      }
      case SDL_CONTROLLERAXISMOTION:
        if (ev.caxis.axis < SDL_CONTROLLER_AXIS_MAX) axes[ev.caxis.axis] = ev.caxis.value;
        break;
      case SDL_CONTROLLERBUTTONDOWN:
      case SDL_CONTROLLERBUTTONUP:
        if (ev.cbutton.button < SDL_CONTROLLER_BUTTON_MAX)
          on_button(ev.cbutton.button, ev.type == SDL_CONTROLLERBUTTONDOWN);
        break;
      }
    }
    double t = now_ms();
    float dt = (float)((t - last) / 1000.0);
    if (dt > 0.1f) dt = 0.1f;
    last = t;
    update_input(dt);

    glClear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    j_UpdateAndRender(env, thiz);
    if (want_shot && glReadPixels_) take_screenshot();
    // Every 10 s: centre pixel of the finished frame, the framebuffer left bound
    // and the GL error flag, which show whether the game really reaches the screen.
    unsigned char px[4] = { 0 };
    int fb = -1;
    int report = t - fps_t >= 10000;
    if (report) {
      if (glGetIntegerv_) glGetIntegerv_(0x8CA6, &fb); // GL_FRAMEBUFFER_BINDING
      if (glReadPixels_) glReadPixels_(scr_w / 2, scr_h / 2, 1, 1, 0x1908, 0x1401, px);
    }
    draw_cursor();
    pSDL_GL_SwapWindow(window);

    frames++;
    heartbeat++;
    if (report) {
      LOG("fps %.1f | RSS %ld MB, free %ld MB | centre %02x%02x%02x | fb %d | glError %#x",
          frames * 1000.0 / (t - fps_t), rss_mb(), avail_mb(), px[0], px[1], px[2], fb,
          glGetError_ ? glGetError_() : 0);
      frames = 0;
      fps_t = t;
    }
    // Without vsync the swap returns at once: hold to ~60 fps.
    double spent = now_ms() - t;
    if (spent < 15) pSDL_Delay((uint32_t)(16 - spent));
  }

  LOG("quitting");
  if (j_ResignActive) j_ResignActive(env, thiz);
  j_UpdateAndRender(env, thiz);
  rb2_quit(0);
}
