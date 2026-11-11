// bionic -> glibc shims for the imports whose ABI differs between the two
// libcs on aarch64. Everything else resolves straight to glibc via dlsym.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "common.h"

// ---- stack guard ------------------------------------------------------------
// Bionic code reads its stack canary from TLS slot 5 (tpidr_el0 + 0x28). On
// glibc that address is the start of the executable's static TLS block + 0x18,
// so the first (and only) TLS variable of this executable puts a constant
// there, which every new thread inherits from .tdata.
__attribute__((aligned(16))) __thread volatile uint64_t bionic_tls[8] = {
  0, 0, 0, 0x2f8dd23a5b3e9c71ull, 0, 0, 0, 0,
};

// ---- setjmp / longjmp -------------------------------------------------------
// bionic's jmp_buf is 256 bytes, glibc's is larger: own implementation that
// stores callee-saved registers only (168 bytes).
__asm__(
  ".text\n.global rb2_setjmp\n.type rb2_setjmp,%function\nrb2_setjmp:\n"
  "  stp x19, x20, [x0, #0]\n  stp x21, x22, [x0, #16]\n  stp x23, x24, [x0, #32]\n"
  "  stp x25, x26, [x0, #48]\n  stp x27, x28, [x0, #64]\n  stp x29, x30, [x0, #80]\n"
  "  mov x2, sp\n  str x2, [x0, #96]\n"
  "  stp d8, d9, [x0, #104]\n  stp d10, d11, [x0, #120]\n"
  "  stp d12, d13, [x0, #136]\n  stp d14, d15, [x0, #152]\n"
  "  mov w0, #0\n  ret\n"
  ".global rb2_longjmp\n.type rb2_longjmp,%function\nrb2_longjmp:\n"
  "  ldp x19, x20, [x0, #0]\n  ldp x21, x22, [x0, #16]\n  ldp x23, x24, [x0, #32]\n"
  "  ldp x25, x26, [x0, #48]\n  ldp x27, x28, [x0, #64]\n  ldp x29, x30, [x0, #80]\n"
  "  ldr x2, [x0, #96]\n  mov sp, x2\n"
  "  ldp d8, d9, [x0, #104]\n  ldp d10, d11, [x0, #120]\n"
  "  ldp d12, d13, [x0, #136]\n  ldp d14, d15, [x0, #152]\n"
  "  cmp w1, #0\n  csinc w0, w1, wzr, ne\n  ret\n");
int rb2_setjmp(void *buf);
void rb2_longjmp(void *buf, int val);

// ---- stdio: bionic's stdin/stdout/stderr are &__sF[0..2] ---------------------
#define BIONIC_FILE_SIZE 152
static char fake_sF[3][BIONIC_FILE_SIZE];

// Every FILE* the game got from fopen/fdopen; anything else reaching a stdio
// wrapper is a bug (bad layout assumption or use after fclose): log its caller.
#define FILE_SET 4096
static FILE *open_files[FILE_SET];
static pthread_mutex_t files_lock = PTHREAD_MUTEX_INITIALIZER;

static void file_track(FILE *f, int add) {
  if (!f) return;
  pthread_mutex_lock(&files_lock);
  unsigned h = ((uintptr_t)f >> 4) % FILE_SET;
  if (add) {
    for (int i = 0; i < FILE_SET; i++, h = (h + 1) % FILE_SET)
      if (!open_files[h] || open_files[h] == (FILE *)1) { open_files[h] = f; break; }
  } else {
    for (int i = 0; i < FILE_SET && open_files[h]; i++, h = (h + 1) % FILE_SET)
      if (open_files[h] == f) { open_files[h] = (FILE *)1; break; }
  }
  pthread_mutex_unlock(&files_lock);
}

static int file_known(FILE *f) {
  int r = 0;
  pthread_mutex_lock(&files_lock);
  unsigned h = ((uintptr_t)f >> 4) % FILE_SET;
  for (int i = 0; i < FILE_SET && open_files[h]; i++, h = (h + 1) % FILE_SET)
    if (open_files[h] == f) { r = 1; break; }
  pthread_mutex_unlock(&files_lock);
  return r;
}

static FILE *mf_(FILE *f, void *ra, const char *fn) {
  uintptr_t p = (uintptr_t)f, b = (uintptr_t)fake_sF;
  if (p >= b && p < b + sizeof fake_sF) {
    switch ((p - b) / BIONIC_FILE_SIZE) {
    case 0: return stdin;
    case 1: return stdout;
    default: return stderr;
    }
  }
  if (f && f != stdin && f != stdout && f != stderr && !file_known(f)) {
    const char *mn = "?";
    uintptr_t off = 0;
    so_addr_info((uintptr_t)ra, &mn, &off);
    LOG("WARNING: %s() on unknown FILE* %p from %s+%#lx (tid %ld)", fn, (void *)f, mn,
        (unsigned long)off, (long)syscall(SYS_gettid));
  }
  return f;
}
#define mf(f) mf_(f, __builtin_return_address(0), __func__ + 2)

static int w_fclose(FILE *f) {
  FILE *r = mf(f);
  if (r != f) return 0;
  file_track(f, 0);
  VLOG("fclose(%p)", (void *)f);
  return fclose(f);
}
static FILE *w_fdopen(int fd, const char *mode) {
  FILE *f = fdopen(fd, mode);
  file_track(f, 1);
  VLOG("fdopen(%d, %s) -> %p", fd, mode, (void *)f);
  return f;
}
static void w_clearerr(FILE *f) { clearerr(mf(f)); }
static int w_feof(FILE *f) { return feof(mf(f)); }
static int w_ferror(FILE *f) { return ferror(mf(f)); }
static int w_fflush(FILE *f) { return fflush(f ? mf(f) : NULL); }
static int w_fgetc(FILE *f) { return fgetc(mf(f)); }
static int w_getc(FILE *f) { return getc(mf(f)); }
static char *w_fgets(char *s, int n, FILE *f) { return fgets(s, n, mf(f)); }
static int w_fileno(FILE *f) { return fileno(mf(f)); }
static int w_fputc(int c, FILE *f) { return fputc(c, mf(f)); }
static int w_putc(int c, FILE *f) { return putc(c, mf(f)); }
static int w_fputs(const char *s, FILE *f) { return fputs(s, mf(f)); }
static size_t w_fread(void *p, size_t s, size_t n, FILE *f) { return fread(p, s, n, mf(f)); }
static size_t w_fwrite(const void *p, size_t s, size_t n, FILE *f) { return fwrite(p, s, n, mf(f)); }
static int w_fseek(FILE *f, long o, int w) { return fseek(mf(f), o, w); }
static int w_fseeko(FILE *f, off_t o, int w) { return fseeko(mf(f), o, w); }
static long w_ftell(FILE *f) { return ftell(mf(f)); }
static off_t w_ftello(FILE *f) { return ftello(mf(f)); }
static void w_rewind(FILE *f) { rewind(mf(f)); }
static int w_ungetc(int c, FILE *f) { return ungetc(c, mf(f)); }
static int w_vfprintf(FILE *f, const char *fmt, va_list ap) { return vfprintf(mf(f), fmt, ap); }
static int w_fprintf(FILE *f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vfprintf(mf(f), fmt, ap);
  va_end(ap);
  return r;
}
static int w_setvbuf(FILE *f, char *b, int m, size_t s) { return setvbuf(mf(f), b, m, s); }

static FILE *w_fopen(const char *path, const char *mode) {
  FILE *f = fopen(path, mode);
  file_track(f, 1);
  if ((g_verbose && f) || (!f && strchr(mode, 'w'))) LOG("fopen(%s, %s) -> %p", path, mode, (void *)f);
  return f;
}

static int w_open(const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & (O_CREAT | O_TMPFILE)) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
  }
  int fd = open(path, flags, mode);
  VLOG("open(%s, %#x) -> %d", path, flags, fd);
  return fd;
}

// ---- stat: same kernel layout on both libcs; glibc < 2.33 lacks stat() --------
static int w_stat(const char *p, struct stat *st) { return syscall(SYS_newfstatat, AT_FDCWD, p, st, 0); }
static int w_lstat(const char *p, struct stat *st) {
  return syscall(SYS_newfstatat, AT_FDCWD, p, st, AT_SYMLINK_NOFOLLOW);
}
static int w_fstat(int fd, struct stat *st) { return syscall(SYS_fstat, fd, st); }
static int w_fstatat(int dfd, const char *p, struct stat *st, int fl) {
  return syscall(SYS_newfstatat, dfd, p, st, fl);
}

// ---- pthread mutex: bionic's is 40 bytes, glibc's 48 -> keep a pointer -------
// bionic mutexes and condition variables are only 4-byte aligned (int32 arrays),
// so the pointer goes in the first 8-aligned slot after the state word: an
// unaligned 64-bit atomic is a SIGBUS on arm64.
static int32_t *state_of(void *p) { return p; }
static void **slot_of(void *p) { return (void **)(((uintptr_t)p + 4 + 7) & ~(uintptr_t)7); }

static pthread_mutex_t *mx(void *p) {
  void **slot = slot_of(p);
  pthread_mutex_t *r = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (r) return r;
  pthread_mutex_t *n = malloc(sizeof *n);
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  // bionic static initializers: 0x4000 recursive, 0x8000 error-checking.
  int32_t st = *state_of(p);
  if (st & 0x4000) pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  else if (st & 0x8000) pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
  pthread_mutex_init(n, &a);
  pthread_mutexattr_destroy(&a);
  void *expected = NULL;
  if (__atomic_compare_exchange_n(slot, &expected, n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return n;
  pthread_mutex_destroy(n);
  free(n);
  return expected;
}

static int w_mutex_init(void *m, const pthread_mutexattr_t *attr) {
  pthread_mutex_t *n = malloc(sizeof *n);
  pthread_mutex_init(n, attr);
  *state_of(m) = 0;
  __atomic_store_n(slot_of(m), n, __ATOMIC_RELEASE);
  return 0;
}
static int w_mutex_destroy(void *m) {
  pthread_mutex_t *r = __atomic_exchange_n(slot_of(m), NULL, __ATOMIC_ACQ_REL);
  if (r) { pthread_mutex_destroy(r); free(r); }
  return 0;
}
static int w_mutex_lock(void *m) { return pthread_mutex_lock(mx(m)); }
static int w_mutex_unlock(void *m) { return pthread_mutex_unlock(mx(m)); }
static int w_mutex_trylock(void *m) { return pthread_mutex_trylock(mx(m)); }
// pthread_cond: glibc's needs 8-byte alignment for its 64-bit atomics, so it
// lives behind a pointer too (same slot layout as the mutex).
static pthread_cond_t *cv(void *p) {
  void **slot = slot_of(p);
  pthread_cond_t *r = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (r) return r;
  pthread_cond_t *n = malloc(sizeof *n);
  pthread_cond_init(n, NULL);
  void *expected = NULL;
  if (__atomic_compare_exchange_n(slot, &expected, n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return n;
  pthread_cond_destroy(n);
  free(n);
  return expected;
}
static int w_cond_init(void *c, const void *attr) {
  (void)attr;
  pthread_cond_t *n = malloc(sizeof *n);
  pthread_cond_init(n, NULL);
  *state_of(c) = 0;
  __atomic_store_n(slot_of(c), n, __ATOMIC_RELEASE);
  return 0;
}
static int w_cond_destroy(void *c) {
  pthread_cond_t *r = __atomic_exchange_n(slot_of(c), NULL, __ATOMIC_ACQ_REL);
  if (r) { pthread_cond_destroy(r); free(r); }
  return 0;
}
static int w_cond_signal(void *c) { return pthread_cond_signal(cv(c)); }
static int w_cond_broadcast(void *c) { return pthread_cond_broadcast(cv(c)); }
static int w_cond_wait(void *c, void *m) { return pthread_cond_wait(cv(c), mx(m)); }
static int w_cond_timedwait(void *c, void *m, const struct timespec *t) {
  return pthread_cond_timedwait(cv(c), mx(m), t);
}

// Game threads are started through a trampoline that gives them an alternate
// signal stack (so a stack overflow still reaches the crash handler) and
// records their tid for the hang watchdog.
#define MAX_THREADS 64
static volatile int thread_tids[MAX_THREADS];
static volatile int thread_count;

int bionic_thread_tids(int *out, int max) {
  int n = thread_count < max ? thread_count : max;
  for (int i = 0; i < n; i++) out[i] = thread_tids[i];
  return n;
}

void rb2_altstack(void) {
  stack_t ss = { .ss_sp = malloc(65536), .ss_size = 65536 };
  if (ss.ss_sp) sigaltstack(&ss, NULL);
}

typedef struct { void *(*fn)(void *); void *arg; } thread_start;

static void *thread_tramp(void *p) {
  thread_start s = *(thread_start *)p;
  free(p);
  rb2_altstack();
  int i = __atomic_fetch_add(&thread_count, 1, __ATOMIC_ACQ_REL);
  if (i < MAX_THREADS) thread_tids[i] = (int)syscall(SYS_gettid);
  return s.fn(s.arg);
}

static int w_pthread_create(pthread_t *t, const void *battr, void *(*fn)(void *), void *arg) {
  thread_start *s = malloc(sizeof *s);
  s->fn = fn;
  s->arg = arg;
  int r = pthread_create(t, NULL, thread_tramp, s);
  if (r) free(s);
  const char *mn = "?";
  uintptr_t off = 0;
  so_addr_info((uintptr_t)fn, &mn, &off);
  LOG("pthread_create start=%s+%#lx -> %d", mn, (unsigned long)off, r);
  // bionic pthread_attr_t: uint32 flags first, bit 0 = detached.
  if (!r && battr && (*(const uint32_t *)battr & 1)) pthread_detach(*t);
  return r;
}

// ---- signals: the game's crash handler is not wanted, sigset_t is 8 bytes ---
static int w_sigaction(int sig, const void *act, void *old) {
  (void)act;
  VLOG("sigaction(%d) ignored", sig);
  if (old) memset(old, 0, 32);
  return 0;
}
static void *w_signal(int sig, void *h) { (void)h; VLOG("signal(%d) ignored", sig); return NULL; }
static int w_sigemptyset(uint64_t *s) { *s = 0; return 0; }
static int w_sigaddset(uint64_t *s, int sig) {
  if (sig < 1 || sig > 64) { errno = EINVAL; return -1; }
  *s |= 1ull << (sig - 1);
  return 0;
}

// ---- sysconf: bionic numbers its _SC_ constants differently -----------------
static long w_sysconf(int name) {
  switch (name) {
  case 0x06: return sysconf(_SC_CLK_TCK);
  case 0x27: case 0x28: return sysconf(_SC_PAGESIZE);
  case 0x60: return sysconf(_SC_NPROCESSORS_CONF);
  case 0x61: return sysconf(_SC_NPROCESSORS_ONLN);
  case 0x62: return sysconf(_SC_PHYS_PAGES);
  case 0x63: return sysconf(_SC_AVPHYS_PAGES);
  }
  LOG("sysconf(%#x) unknown", name);
  return -1;
}

// ---- misc bionic-only symbols ----------------------------------------------
static int *w_errno(void) { return __errno_location(); }
static size_t w_strlen_chk(const char *s, size_t n) { (void)n; return strlen(s); }
static char *w_strncpy_chk2(char *d, const char *s, size_t n, size_t dl, size_t sl) {
  (void)dl; (void)sl;
  return strncpy(d, s, n);
}
static mode_t w_umask_chk(mode_t m) { return umask(m); }
static int w_open_2(const char *p, int f) { return open(p, f); }
static ssize_t w_read_chk(int fd, void *b, size_t n, size_t bl) { (void)bl; return read(fd, b, n); }
static void w_stack_chk_fail(void) {
  void *ra = __builtin_return_address(0);
  const char *mn = "?";
  uintptr_t off = 0;
  so_addr_info((uintptr_t)ra, &mn, &off);
  LOG("stack smashing detected in %s+%#lx", mn, (unsigned long)off);
  abort();
}
static void w_abort_msg(const char *msg) { LOG("abort message: %s", msg); }
static int w_log_write(int prio, const char *tag, const char *msg) {
  if (prio >= 4 || g_verbose) fprintf(stderr, "[%s] %s\n", tag ? tag : "log", msg ? msg : "");
  return 1;
}
static int w_log_print(int prio, const char *tag, const char *fmt, ...) {
  if (prio < 4 && !g_verbose) return 1;
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[%s] ", tag ? tag : "log");
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  return 1;
}
static int w_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
  if (prio < 4 && !g_verbose) return 1;
  fprintf(stderr, "[%s] ", tag ? tag : "log");
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  return 1;
}
static void w_exit(int code) { LOG("game called exit(%d)", code); rb2_quit(code); }
static int (*p_xpg_strerror_r)(int, char *, size_t);
static int w_strerror_r(int e, char *b, size_t n) {
  if (p_xpg_strerror_r) return p_xpg_strerror_r(e, b, n);
  snprintf(b, n, "error %d", e);
  return 0;
}
static int w_system_property_get(const char *name, char *value) { (void)name; value[0] = 0; return 0; }

static const struct { const char *name; void *fn; } shims[] = {
  { "__sF", fake_sF },
  { "fclose", w_fclose }, { "clearerr", w_clearerr }, { "feof", w_feof }, { "ferror", w_ferror },
  { "fflush", w_fflush }, { "fgetc", w_fgetc }, { "getc", w_getc }, { "fgets", w_fgets },
  { "fileno", w_fileno }, { "fputc", w_fputc }, { "putc", w_putc }, { "fputs", w_fputs },
  { "fread", w_fread }, { "fwrite", w_fwrite }, { "fseek", w_fseek }, { "fseeko", w_fseeko },
  { "ftell", w_ftell }, { "ftello", w_ftello }, { "rewind", w_rewind }, { "ungetc", w_ungetc },
  { "vfprintf", w_vfprintf }, { "fprintf", w_fprintf }, { "setvbuf", w_setvbuf },
  { "fopen", w_fopen }, { "fdopen", w_fdopen }, { "open", w_open },
  { "stat", w_stat }, { "lstat", w_lstat }, { "fstat", w_fstat }, { "fstatat", w_fstatat },
  { "pthread_mutex_init", w_mutex_init }, { "pthread_mutex_destroy", w_mutex_destroy },
  { "pthread_mutex_lock", w_mutex_lock }, { "pthread_mutex_unlock", w_mutex_unlock },
  { "pthread_mutex_trylock", w_mutex_trylock }, { "pthread_cond_wait", w_cond_wait },
  { "pthread_cond_timedwait", w_cond_timedwait }, { "pthread_cond_init", w_cond_init },
  { "pthread_cond_destroy", w_cond_destroy }, { "pthread_cond_signal", w_cond_signal },
  { "pthread_cond_broadcast", w_cond_broadcast }, { "pthread_create", w_pthread_create },
  { "sigaction", w_sigaction }, { "signal", w_signal }, { "bsd_signal", w_signal },
  { "sigemptyset", w_sigemptyset }, { "sigaddset", w_sigaddset },
  { "sysconf", w_sysconf },
  { "setjmp", rb2_setjmp }, { "_setjmp", rb2_setjmp }, { "sigsetjmp", rb2_setjmp },
  { "longjmp", rb2_longjmp }, { "_longjmp", rb2_longjmp }, { "siglongjmp", rb2_longjmp },
  { "__errno", w_errno }, { "__strlen_chk", w_strlen_chk }, { "__strncpy_chk2", w_strncpy_chk2 },
  { "__umask_chk", w_umask_chk }, { "__open_2", w_open_2 }, { "__read_chk", w_read_chk },
  { "__stack_chk_fail", w_stack_chk_fail }, { "android_set_abort_message", w_abort_msg },
  { "__android_log_write", w_log_write }, { "__android_log_print", w_log_print },
  { "__android_log_vprint", w_log_vprint },
  { "exit", w_exit }, { "strerror_r", w_strerror_r },
  { "__system_property_get", w_system_property_get },
  { "dl_iterate_phdr", so_iterate_phdr },
};

void bionic_init(void) {
  // The game's math imports resolve through RTLD_DEFAULT, so libm must be global.
  if (!dlopen("libm.so.6", RTLD_NOW | RTLD_GLOBAL)) LOG("cannot load libm: %s", dlerror());
  p_xpg_strerror_r = dlsym(RTLD_DEFAULT, "__xpg_strerror_r");
  uintptr_t tp;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  if ((uintptr_t)&bionic_tls[3] != tp + 0x28)
    LOG("WARNING: stack guard slot at %p, expected %p", (void *)&bionic_tls[3], (void *)(tp + 0x28));
}

void *bionic_resolve(const char *name) {
  for (size_t i = 0; i < sizeof shims / sizeof shims[0]; i++)
    if (!strcmp(shims[i].name, name)) return shims[i].fn;
  return NULL;
}
