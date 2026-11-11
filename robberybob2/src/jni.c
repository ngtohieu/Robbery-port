// A small fake JVM: JNIEnv/JavaVM tables, string/array objects, and the Java
// side of Robbery Bob 2 (methods the game registers through RegisterJavaMethod
// and calls back by name).
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include "common.h"

typedef union { uint8_t z; int8_t b; uint16_t c; int16_t s; int32_t i; int64_t j; float f; double d; void *l; } jvalue;

enum { K_OBJ, K_CLASS, K_STRING, K_BYTES, K_INTS, K_FLOATS, K_OBJS, K_SHORTS, K_LONGS, K_DOUBLES, K_BOOLS, K_CHARS };
#define JMAGIC 0x4a4f424au

struct jobj {
  uint32_t magic;
  int kind;
  int len;           // array length / string byte length
  char *cls;         // class name (slash form)
  void *data;        // string bytes / array elements
};

typedef struct jmeth { char *cls, *name, *sig; struct jmeth *next; } jmeth;
static jmeth *meths;
static pthread_mutex_t jlock = PTHREAD_MUTEX_INITIALIZER;

static int elem_size(int k) {
  switch (k) {
  case K_BYTES: case K_BOOLS: return 1;
  case K_SHORTS: case K_CHARS: return 2;
  case K_INTS: case K_FLOATS: return 4;
  default: return 8;
  }
}

static jobj *new_obj(int kind, const char *cls) {
  jobj *o = calloc(1, sizeof *o);
  o->magic = JMAGIC;
  o->kind = kind;
  o->cls = strdup(cls ? cls : "java/lang/Object");
  return o;
}
static int valid(jobj *o) { return o && (uintptr_t)o > 0x10000 && o->magic == JMAGIC; }

jobj *jni_new_object(const char *cls) { return new_obj(K_OBJ, cls); }
jobj *jni_new_string(const char *s) {
  jobj *o = new_obj(K_STRING, "java/lang/String");
  o->data = strdup(s ? s : "");
  o->len = strlen(o->data);
  return o;
}
static jobj *new_array(int kind, int len, const char *cls) {
  jobj *o = new_obj(kind, cls);
  o->len = len;
  o->data = calloc(len ? len : 1, elem_size(kind));
  return o;
}
jobj *jni_new_int_array(int len) { return new_array(K_INTS, len, "[I"); }
int *jni_int_data(jobj *a) { return a->data; }
const char *jni_string(jobj *s) { return valid(s) && s->kind == K_STRING ? s->data : NULL; }

static jobj *find_class(const char *name) {
  static jobj *classes[256];
  static int nclasses;
  pthread_mutex_lock(&jlock);
  for (int i = 0; i < nclasses; i++)
    if (!strcmp(classes[i]->cls, name)) { pthread_mutex_unlock(&jlock); return classes[i]; }
  jobj *c = new_obj(K_CLASS, name);
  if (nclasses < 256) classes[nclasses++] = c;
  pthread_mutex_unlock(&jlock);
  return c;
}

static jmeth *get_meth(const char *cls, const char *name, const char *sig) {
  pthread_mutex_lock(&jlock);
  for (jmeth *m = meths; m; m = m->next)
    if (!strcmp(m->name, name) && !strcmp(m->sig, sig) && !strcmp(m->cls, cls)) {
      pthread_mutex_unlock(&jlock);
      return m;
    }
  jmeth *m = calloc(1, sizeof *m);
  m->cls = strdup(cls);
  m->name = strdup(name);
  m->sig = strdup(sig);
  m->next = meths;
  meths = m;
  pthread_mutex_unlock(&jlock);
  VLOG("jni: method id %s.%s%s", cls, name, sig);
  return m;
}

// ---- save files (leGameSpecificData.internalSave/internalLoad) ---------------
static void save_path(char *out, size_t n, const char *name) {
  const char *b = strrchr(name, '/');
  snprintf(out, n, "%s/%s", g_savedir, b ? b + 1 : name);
}
static void internal_save(const char *name, jobj *bytes) {
  if (!name || !valid(bytes)) return;
  char p[1024], t[1100];
  save_path(p, sizeof p, name);
  snprintf(t, sizeof t, "%s.tmp", p);
  FILE *f = fopen(t, "wb");
  if (!f) { LOG("save: cannot write %s", t); return; }
  size_t w = fwrite(bytes->data, 1, bytes->len, f);
  fflush(f);
  fsync(fileno(f));
  fclose(f);
  if (w == (size_t)bytes->len && rename(t, p) == 0) LOG("save: %s (%d bytes)", p, bytes->len);
  else LOG("save: failed for %s", p);
}
static jobj *internal_load(const char *name) {
  if (!name) return NULL;
  char p[1024];
  save_path(p, sizeof p, name);
  FILE *f = fopen(p, "rb");
  if (!f) { LOG("load: %s not found", p); return NULL; }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  jobj *a = new_array(K_BYTES, (int)n, "[B");
  if (fread(a->data, 1, n, f) != (size_t)n) LOG("load: short read on %s", p);
  fclose(f);
  LOG("load: %s (%ld bytes)", p, n);
  return a;
}

// ---- Firebase remote config defaults (res/xml/remote_config_defaults.xml) ----
static const char *const config_defaults[][2] = {
  { "isLifesFeatureEnabled", "false" },
  { "Autumn_unlock_requirement_free", "stars,40,levels,20" },
  { "Docks_unlock_requirement_free", "stars,80,levels,40" },
  { "Resort_unlock_requirement_free", "stars,120,levels,60" },
  { "Halloween_unlock_requirement_free", "stars,160,levels,80" },
  { "paywall_mode", "" },
  { "double_coins_mode", "" },
  { "ab_ad_pacing", "" },
};

static const char *str_arg(jvalue *a, int i) { return jni_string(a[i].l); }

// ---- the Java side -------------------------------------------------------------
static jvalue java_call(jobj *obj, jmeth *m, jvalue *a) {
  jvalue r;
  r.j = 0;
  const char *n = m->name, *sig = m->sig;
  (void)obj;
#define IS(x) (!strcmp(n, x))
  if (IS("GetAssetsPath")) r.l = jni_new_string(g_apk);
  else if (IS("internalSave")) internal_save(str_arg(a, 0), a[1].l);
  else if (IS("internalLoad")) r.l = internal_load(str_arg(a, 0));
  // sound (leSoundManager)
  else if (IS("loadMusic")) r.z = audio_load_music(str_arg(a, 0));
  else if (IS("playMusic")) audio_play_music(a[0].z);
  else if (IS("stopMusic")) audio_stop_music();
  else if (IS("isPlayingMusic")) r.z = audio_music_playing();
  else if (IS("setMusicVolume")) audio_music_volume(a[0].f);
  else if (IS("getMusicCurrentTime")) r.f = audio_music_time();
  else if (IS("setMusicCurrentTime")) audio_set_music_time(a[0].f);
  else if (IS("loadSoundEffect")) r.i = audio_load_sound(str_arg(a, 0));
  else if (IS("unloadSound")) audio_unload_sound(a[1].i);
  else if (IS("playSoundEffect")) r.i = audio_play_sound(a[0].i, a[1].i, 1.0f, 0);
  else if (IS("playSoundEffectWithVolume")) r.i = audio_play_sound(a[0].i, a[1].i, a[2].f, 1);
  else if (IS("stopSound")) audio_stop_sound(a[0].i);
  else if (IS("isSoundEffectPlaying")) r.z = audio_sound_playing(a[0].i);
  else if (IS("setSoundVolume")) {
    if (!strcmp(sig, "(F)V")) audio_global_sound_volume(a[0].f);
    else audio_sound_volume(a[0].i, a[1].f);
  }
  else if (IS("setRate")) { /* pitch changes are not supported */ }
  else if (IS("getSoundDuration")) r.f = audio_sound_duration(str_arg(a, 0));
  // activity
  else if (IS("QuitGame") || IS("OnQuitGameComplete")) { LOG("game requested quit (%s)", n); g_quit_requested = 1; }
  else if (IS("IsAgeVerificationCriteriaMet")) r.z = 1;
  else if (IS("UseOBB")) r.z = 0;
  else if (IS("GetDeviceLocale")) {
    const char *l = getenv("RB2_LANG");
    r.l = jni_new_string(l && *l ? l : "en");
  }
  else if (IS("GetLocaleCountryCode")) r.l = jni_new_string("US");
  else if (IS("isLifesFeatureEnabled")) r.z = 0;
  else if (IS("GetConfigValueForKey")) {
    const char *k = str_arg(a, 0), *v = "";
    for (size_t i = 0; k && i < sizeof config_defaults / sizeof config_defaults[0]; i++)
      if (!strcmp(config_defaults[i][0], k)) v = config_defaults[i][1];
    r.l = jni_new_string(v);
  }
  else if (IS("IsSignedIn") || IS("IsRewardedVideoAvailable") || IS("IsInterstitialAvailable") ||
           IS("IsOfferwallAvailable") || IS("StopYoutubePlayer") || IS("ResumeYoutubePlayer") ||
           IS("StartYoutubePlaylistWithID") || IS("StartVideoWithID")) r.z = 0;
  else {
    // Analytics, ads, billing, game services, URL requests: nothing to do.
    static const char *quiet[] = { "AddKeyAndValue", "LogEventWithName", "ClearParameterCache",
                                   "AddResourceEvent", "AddProgressionEvent", "GameInitComplete",
                                   "RemoveSplashes", "MakeSureLoaderThreadIsRunning", "SC" };
    int q = 0;
    for (size_t i = 0; i < sizeof quiet / sizeof quiet[0]; i++)
      if (!strncmp(n, quiet[i], strlen(quiet[i]))) q = 1;
    if (!q || g_verbose) LOG("jni: call %s.%s%s (ignored)", m->cls, n, sig);
    const char *ret = strchr(sig, ')');
    if (ret && ret[1] == 'L' && !strcmp(ret + 1, "Ljava/lang/String;")) r.l = jni_new_string("");
  }
#undef IS
  return r;
}

// ---- argument decoding ---------------------------------------------------------
static const char *next_type(const char *p) {
  while (*p == '[') p++;
  if (*p == 'L') { while (*p && *p != ';') p++; }
  return *p ? p + 1 : p;
}
static int count_args(const char *sig) {
  int n = 0;
  for (const char *p = sig + 1; *p && *p != ')'; p = next_type(p)) n++;
  return n;
}
static void args_from_va(const char *sig, va_list ap, jvalue *out) {
  int i = 0;
  for (const char *p = sig + 1; *p && *p != ')' && i < 16; p = next_type(p), i++) {
    switch (*p) {
    case 'Z': case 'B': case 'C': case 'S': case 'I': out[i].j = 0; out[i].i = va_arg(ap, int); break;
    case 'J': out[i].j = va_arg(ap, long); break;
    case 'F': out[i].j = 0; out[i].f = (float)va_arg(ap, double); break;
    case 'D': out[i].d = va_arg(ap, double); break;
    default: out[i].l = va_arg(ap, void *); break;
    }
  }
}

static jvalue invoke_v(jobj *obj, jmeth *m, va_list ap) {
  jvalue a[16];
  memset(a, 0, sizeof a);
  args_from_va(m->sig, ap, a);
  return java_call(obj, m, a);
}
static jvalue invoke_a(jobj *obj, jmeth *m, const jvalue *args) {
  jvalue a[16];
  memset(a, 0, sizeof a);
  int n = count_args(m->sig);
  if (n > 16) n = 16;
  if (args) memcpy(a, args, n * sizeof(jvalue));
  return java_call(obj, m, a);
}

// ---- JNIEnv functions ----------------------------------------------------------
typedef void *E; // JNIEnv*
static int GetVersion(E e) { (void)e; return 0x10006; }
static jobj *FindClass(E e, const char *n) { (void)e; VLOG("jni: FindClass %s", n); return find_class(n); }
static jobj *GetSuperclass(E e, jobj *c) { (void)e; (void)c; return find_class("java/lang/Object"); }
static uint8_t IsAssignableFrom(E e, jobj *a, jobj *b) { (void)e; (void)a; (void)b; return 1; }
static int Throw(E e, jobj *o) { (void)e; (void)o; LOG("jni: Throw"); return 0; }
static int ThrowNew(E e, jobj *c, const char *msg) { (void)e; LOG("jni: ThrowNew %s: %s", valid(c) ? c->cls : "?", msg); return 0; }
static jobj *ExceptionOccurred(E e) { (void)e; return NULL; }
static void ExceptionNoop(E e) { (void)e; }
static void FatalError(E e, const char *msg) { (void)e; LOG("jni: FatalError %s", msg); abort(); }
static int PushLocalFrame(E e, int n) { (void)e; (void)n; return 0; }
static jobj *PopLocalFrame(E e, jobj *r) { (void)e; return r; }
static jobj *NewRef(E e, jobj *o) { (void)e; return o; }
static void DeleteRef(E e, jobj *o) { (void)e; (void)o; }
static uint8_t IsSameObject(E e, jobj *a, jobj *b) { (void)e; return a == b; }
static int EnsureLocalCapacity(E e, int n) { (void)e; (void)n; return 0; }
static jobj *AllocObject(E e, jobj *c) { (void)e; return new_obj(K_OBJ, valid(c) ? c->cls : NULL); }
static jobj *NewObject(E e, jobj *c, jmeth *m, ...) { (void)e; (void)m; VLOG("jni: NewObject %s", valid(c) ? c->cls : "?"); return new_obj(K_OBJ, valid(c) ? c->cls : NULL); }
static jobj *NewObjectV(E e, jobj *c, jmeth *m, va_list ap) { (void)e; (void)m; (void)ap; return new_obj(K_OBJ, valid(c) ? c->cls : NULL); }
static jobj *NewObjectA(E e, jobj *c, jmeth *m, jvalue *a) { (void)e; (void)m; (void)a; return new_obj(K_OBJ, valid(c) ? c->cls : NULL); }
static jobj *GetObjectClass(E e, jobj *o) { (void)e; return find_class(valid(o) ? o->cls : "java/lang/Object"); }
static uint8_t IsInstanceOf(E e, jobj *o, jobj *c) { (void)e; (void)o; (void)c; return 1; }
static jmeth *GetMethodID(E e, jobj *c, const char *n, const char *s) { (void)e; return get_meth(valid(c) ? c->cls : "?", n, s); }

#define CALLS(Name, T, field) \
  static T Call##Name##Method(E e, jobj *o, jmeth *m, ...) { \
    (void)e; va_list ap; va_start(ap, m); jvalue r = invoke_v(o, m, ap); va_end(ap); return (T)r.field; } \
  static T Call##Name##MethodV(E e, jobj *o, jmeth *m, va_list ap) { (void)e; return (T)invoke_v(o, m, ap).field; } \
  static T Call##Name##MethodA(E e, jobj *o, jmeth *m, jvalue *a) { (void)e; return (T)invoke_a(o, m, a).field; } \
  static T CallNonvirtual##Name##Method(E e, jobj *o, jobj *c, jmeth *m, ...) { \
    (void)e; (void)c; va_list ap; va_start(ap, m); jvalue r = invoke_v(o, m, ap); va_end(ap); return (T)r.field; } \
  static T CallNonvirtual##Name##MethodV(E e, jobj *o, jobj *c, jmeth *m, va_list ap) { (void)e; (void)c; return (T)invoke_v(o, m, ap).field; } \
  static T CallNonvirtual##Name##MethodA(E e, jobj *o, jobj *c, jmeth *m, jvalue *a) { (void)e; (void)c; return (T)invoke_a(o, m, a).field; } \
  static T CallStatic##Name##Method(E e, jobj *c, jmeth *m, ...) { \
    (void)e; va_list ap; va_start(ap, m); jvalue r = invoke_v(c, m, ap); va_end(ap); return (T)r.field; } \
  static T CallStatic##Name##MethodV(E e, jobj *c, jmeth *m, va_list ap) { (void)e; return (T)invoke_v(c, m, ap).field; } \
  static T CallStatic##Name##MethodA(E e, jobj *c, jmeth *m, jvalue *a) { (void)e; return (T)invoke_a(c, m, a).field; }
CALLS(Object, void *, l)
CALLS(Boolean, uint8_t, z)
CALLS(Byte, int8_t, b)
CALLS(Char, uint16_t, c)
CALLS(Short, int16_t, s)
CALLS(Int, int32_t, i)
CALLS(Long, int64_t, j)
CALLS(Float, float, f)
CALLS(Double, double, d)
static void CallVoidMethod(E e, jobj *o, jmeth *m, ...) { (void)e; va_list ap; va_start(ap, m); invoke_v(o, m, ap); va_end(ap); }
static void CallVoidMethodV(E e, jobj *o, jmeth *m, va_list ap) { (void)e; invoke_v(o, m, ap); }
static void CallVoidMethodA(E e, jobj *o, jmeth *m, jvalue *a) { (void)e; invoke_a(o, m, a); }
static void CallNonvirtualVoidMethod(E e, jobj *o, jobj *c, jmeth *m, ...) { (void)e; (void)c; va_list ap; va_start(ap, m); invoke_v(o, m, ap); va_end(ap); }
static void CallNonvirtualVoidMethodV(E e, jobj *o, jobj *c, jmeth *m, va_list ap) { (void)e; (void)c; invoke_v(o, m, ap); }
static void CallNonvirtualVoidMethodA(E e, jobj *o, jobj *c, jmeth *m, jvalue *a) { (void)e; (void)c; invoke_a(o, m, a); }
static void CallStaticVoidMethod(E e, jobj *c, jmeth *m, ...) { (void)e; va_list ap; va_start(ap, m); invoke_v(c, m, ap); va_end(ap); }
static void CallStaticVoidMethodV(E e, jobj *c, jmeth *m, va_list ap) { (void)e; invoke_v(c, m, ap); }
static void CallStaticVoidMethodA(E e, jobj *c, jmeth *m, jvalue *a) { (void)e; invoke_a(c, m, a); }

// Fields: nothing in the game's Java side is read this way; object fields get a
// placeholder object so that calls made on it still reach java_call.
static jmeth *GetFieldID(E e, jobj *c, const char *n, const char *s) {
  (void)e;
  LOG("jni: field %s.%s %s", valid(c) ? c->cls : "?", n, s);
  return get_meth(valid(c) ? c->cls : "?", n, s);
}
static jobj *GetObjectField(E e, jobj *o, jmeth *f) {
  (void)e; (void)o;
  if (f->sig[0] == 'L') {
    char cls[256];
    snprintf(cls, sizeof cls, "%.*s", (int)strlen(f->sig) - 2, f->sig + 1);
    return new_obj(K_OBJ, cls);
  }
  return NULL;
}
static long GetZeroField(E e, jobj *o, jmeth *f) { (void)e; (void)o; (void)f; return 0; }
static float GetFloatField(E e, jobj *o, jmeth *f) { (void)e; (void)o; (void)f; return 0; }
static double GetDoubleField(E e, jobj *o, jmeth *f) { (void)e; (void)o; (void)f; return 0; }
static void SetField(E e, jobj *o, jmeth *f, ...) { (void)e; (void)o; (void)f; }
static void SetFloatField(E e, jobj *o, jmeth *f, float v) { (void)e; (void)o; (void)f; (void)v; }
static void SetDoubleField(E e, jobj *o, jmeth *f, double v) { (void)e; (void)o; (void)f; (void)v; }

static jobj *NewString(E e, const uint16_t *u, int len) {
  (void)e;
  char *s = malloc(len * 3 + 1), *p = s;
  for (int i = 0; i < len; i++) {
    uint16_t c = u[i];
    if (c < 0x80) *p++ = c;
    else if (c < 0x800) { *p++ = 0xc0 | c >> 6; *p++ = 0x80 | (c & 0x3f); }
    else { *p++ = 0xe0 | c >> 12; *p++ = 0x80 | ((c >> 6) & 0x3f); *p++ = 0x80 | (c & 0x3f); }
  }
  *p = 0;
  jobj *o = jni_new_string(s);
  free(s);
  return o;
}
static int utf16_len(const char *s) {
  int n = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    if ((*p & 0xc0) != 0x80) n += (*p >= 0xf0) ? 2 : 1;
  return n;
}
static int GetStringLength(E e, jobj *s) { (void)e; return valid(s) ? utf16_len(s->data) : 0; }
static const uint16_t *GetStringChars(E e, jobj *s, uint8_t *copy) {
  (void)e;
  if (copy) *copy = 1;
  if (!valid(s)) return NULL;
  const unsigned char *p = s->data;
  uint16_t *u = calloc(utf16_len(s->data) + 1, 2), *q = u;
  while (*p) {
    uint32_t c = *p++;
    if (c >= 0xe0 && p[0] && p[1]) { c = (c & 0x0f) << 12 | (p[0] & 0x3f) << 6 | (p[1] & 0x3f); p += 2; }
    else if (c >= 0xc0 && p[0]) { c = (c & 0x1f) << 6 | (p[0] & 0x3f); p++; }
    *q++ = c;
  }
  return u;
}
static void ReleaseStringChars(E e, jobj *s, const uint16_t *u) { (void)e; (void)s; free((void *)u); }
static jobj *NewStringUTF(E e, const char *s) { (void)e; return s ? jni_new_string(s) : NULL; }
static int GetStringUTFLength(E e, jobj *s) { (void)e; return valid(s) ? s->len : 0; }
static const char *GetStringUTFChars(E e, jobj *s, uint8_t *copy) {
  (void)e;
  if (copy) *copy = 0;
  return valid(s) ? s->data : NULL;
}
static void ReleaseStringUTFChars(E e, jobj *s, const char *c) { (void)e; (void)s; (void)c; }
static void GetStringRegion(E e, jobj *s, int start, int len, uint16_t *buf) {
  const uint16_t *u = GetStringChars(e, s, NULL);
  if (u) { memcpy(buf, u + start, len * 2); free((void *)u); }
}
static void GetStringUTFRegion(E e, jobj *s, int start, int len, char *buf) {
  (void)e;
  if (valid(s)) { memcpy(buf, (char *)s->data + start, len); buf[len] = 0; }
}

static int GetArrayLength(E e, jobj *a) { (void)e; return valid(a) ? a->len : 0; }
static jobj *NewObjectArray(E e, int len, jobj *c, jobj *init) {
  (void)e;
  jobj *a = new_array(K_OBJS, len, valid(c) ? c->cls : NULL);
  for (int i = 0; i < len; i++) ((jobj **)a->data)[i] = init;
  return a;
}
static jobj *GetObjectArrayElement(E e, jobj *a, int i) {
  (void)e;
  return valid(a) && i >= 0 && i < a->len ? ((jobj **)a->data)[i] : NULL;
}
static void SetObjectArrayElement(E e, jobj *a, int i, jobj *v) {
  (void)e;
  if (valid(a) && i >= 0 && i < a->len) ((jobj **)a->data)[i] = v;
}
#define ARRAYS(Name, K, sig) \
  static jobj *New##Name##Array(E e, int len) { (void)e; return new_array(K, len, sig); }
ARRAYS(Boolean, K_BOOLS, "[Z") ARRAYS(Byte, K_BYTES, "[B") ARRAYS(Char, K_CHARS, "[C")
ARRAYS(Short, K_SHORTS, "[S") ARRAYS(Int, K_INTS, "[I") ARRAYS(Long, K_LONGS, "[J")
ARRAYS(Float, K_FLOATS, "[F") ARRAYS(Double, K_DOUBLES, "[D")
static void *GetArrayElements(E e, jobj *a, uint8_t *copy) {
  (void)e;
  if (copy) *copy = 0;
  return valid(a) ? a->data : NULL;
}
static void ReleaseArrayElements(E e, jobj *a, void *p, int mode) { (void)e; (void)a; (void)p; (void)mode; }
static void GetArrayRegion(E e, jobj *a, int start, int len, void *buf) {
  (void)e;
  if (!valid(a) || start < 0 || start + len > a->len) return;
  int es = elem_size(a->kind);
  memcpy(buf, (char *)a->data + start * es, len * es);
}
static void SetArrayRegion(E e, jobj *a, int start, int len, const void *buf) {
  (void)e;
  if (!valid(a) || start < 0 || start + len > a->len) return;
  int es = elem_size(a->kind);
  memcpy((char *)a->data + start * es, buf, len * es);
}
static void *GetPrimitiveArrayCritical(E e, jobj *a, uint8_t *copy) { return GetArrayElements(e, a, copy); }
static void ReleasePrimitiveArrayCritical(E e, jobj *a, void *p, int mode) { (void)e; (void)a; (void)p; (void)mode; }
static const uint16_t *GetStringCritical(E e, jobj *s, uint8_t *copy) { return GetStringChars(e, s, copy); }
static void ReleaseStringCritical(E e, jobj *s, const uint16_t *u) { ReleaseStringChars(e, s, u); }

typedef struct { const char *name, *sig; void *fn; } JNINativeMethod;
static int RegisterNatives(E e, jobj *c, const JNINativeMethod *m, int n) {
  (void)e;
  for (int i = 0; i < n; i++) LOG("jni: RegisterNatives %s.%s%s", valid(c) ? c->cls : "?", m[i].name, m[i].sig);
  return 0;
}
static int UnregisterNatives(E e, jobj *c) { (void)e; (void)c; return 0; }
static int Monitor(E e, jobj *o) { (void)e; (void)o; return 0; }
static void *vm_ptr;
static int GetJavaVM(E e, void **vm) { (void)e; *vm = vm_ptr; return 0; }
static uint8_t ExceptionCheck(E e) { (void)e; return 0; }
static jobj *NewDirectByteBuffer(E e, void *p, long n) {
  (void)e;
  jobj *o = new_obj(K_BYTES, "java/nio/ByteBuffer");
  o->data = p;
  o->len = n;
  return o;
}
static void *GetDirectBufferAddress(E e, jobj *o) { (void)e; return valid(o) ? o->data : NULL; }
static long GetDirectBufferCapacity(E e, jobj *o) { (void)e; return valid(o) ? o->len : -1; }
static int GetObjectRefType(E e, jobj *o) { (void)e; (void)o; return 2; }

// ---- table ---------------------------------------------------------------------
#define JNI_SLOTS 233
static void *env_table[JNI_SLOTS];
static void *env_obj = env_table; // JNIEnv is a pointer to the function table
static void *vm_table[8];
static void *vm_obj = vm_table;

static void jni_unimpl(int slot) { LOG("jni: unimplemented JNIEnv slot %d called", slot); }
#define STUB(n) static long jni_stub_##n(void) { jni_unimpl(n); return 0; }
#include "jni_stubs.h"
#undef STUB

static int DestroyJavaVM(void *vm) { (void)vm; return 0; }
static int AttachCurrentThread(void *vm, void **env, void *args) { (void)vm; (void)args; *env = &env_obj; return 0; }
static int DetachCurrentThread(void *vm) { (void)vm; return 0; }
static int GetEnv(void *vm, void **env, int ver) { (void)vm; (void)ver; *env = &env_obj; return 0; }

void *jni_env(void) { return &env_obj; }
void *jni_vm(void) { return &vm_obj; }

void jni_init(void) {
#define STUB(n) env_table[n] = (void *)jni_stub_##n;
#include "jni_stubs.h"
#undef STUB
  void **t = env_table;
  t[4] = GetVersion; t[6] = FindClass; t[10] = GetSuperclass; t[11] = IsAssignableFrom;
  t[13] = Throw; t[14] = ThrowNew; t[15] = ExceptionOccurred; t[16] = ExceptionNoop;
  t[17] = ExceptionNoop; t[18] = FatalError; t[19] = PushLocalFrame; t[20] = PopLocalFrame;
  t[21] = NewRef; t[22] = DeleteRef; t[23] = DeleteRef; t[24] = IsSameObject; t[25] = NewRef;
  t[26] = EnsureLocalCapacity; t[27] = AllocObject; t[28] = NewObject; t[29] = NewObjectV;
  t[30] = NewObjectA; t[31] = GetObjectClass; t[32] = IsInstanceOf; t[33] = GetMethodID;
#define SET3(base, Name) t[base] = Call##Name##Method; t[base + 1] = Call##Name##MethodV; t[base + 2] = Call##Name##MethodA;
#define SETNV(base, Name) t[base] = CallNonvirtual##Name##Method; t[base + 1] = CallNonvirtual##Name##MethodV; t[base + 2] = CallNonvirtual##Name##MethodA;
#define SETST(base, Name) t[base] = CallStatic##Name##Method; t[base + 1] = CallStatic##Name##MethodV; t[base + 2] = CallStatic##Name##MethodA;
#define ALL(M, b) M(b, Object) M(b + 3, Boolean) M(b + 6, Byte) M(b + 9, Char) M(b + 12, Short) \
  M(b + 15, Int) M(b + 18, Long) M(b + 21, Float) M(b + 24, Double) M(b + 27, Void)
  ALL(SET3, 34)
  ALL(SETNV, 64)
  t[94] = GetFieldID;
  t[95] = GetObjectField;
  for (int i = 96; i <= 101; i++) t[i] = GetZeroField;
  t[102] = GetFloatField; t[103] = GetDoubleField;
  for (int i = 104; i <= 110; i++) t[i] = SetField;
  t[111] = SetFloatField; t[112] = SetDoubleField;
  t[113] = GetMethodID; // GetStaticMethodID
  ALL(SETST, 114)
  t[144] = GetFieldID; // GetStaticFieldID
  t[145] = GetObjectField;
  for (int i = 146; i <= 151; i++) t[i] = GetZeroField;
  t[152] = GetFloatField; t[153] = GetDoubleField;
  for (int i = 154; i <= 160; i++) t[i] = SetField;
  t[161] = SetFloatField; t[162] = SetDoubleField;
  t[163] = NewString; t[164] = GetStringLength; t[165] = GetStringChars; t[166] = ReleaseStringChars;
  t[167] = NewStringUTF; t[168] = GetStringUTFLength; t[169] = GetStringUTFChars;
  t[170] = ReleaseStringUTFChars; t[171] = GetArrayLength; t[172] = NewObjectArray;
  t[173] = GetObjectArrayElement; t[174] = SetObjectArrayElement;
  t[175] = NewBooleanArray; t[176] = NewByteArray; t[177] = NewCharArray; t[178] = NewShortArray;
  t[179] = NewIntArray; t[180] = NewLongArray; t[181] = NewFloatArray; t[182] = NewDoubleArray;
  for (int i = 183; i <= 190; i++) t[i] = GetArrayElements;
  for (int i = 191; i <= 198; i++) t[i] = ReleaseArrayElements;
  for (int i = 199; i <= 206; i++) t[i] = GetArrayRegion;
  for (int i = 207; i <= 214; i++) t[i] = SetArrayRegion;
  t[215] = RegisterNatives; t[216] = UnregisterNatives; t[217] = Monitor; t[218] = Monitor;
  t[219] = GetJavaVM; t[220] = GetStringRegion; t[221] = GetStringUTFRegion;
  t[222] = GetPrimitiveArrayCritical; t[223] = ReleasePrimitiveArrayCritical;
  t[224] = GetStringCritical; t[225] = ReleaseStringCritical; t[226] = NewRef; t[227] = DeleteRef;
  t[228] = ExceptionCheck; t[229] = NewDirectByteBuffer; t[230] = GetDirectBufferAddress;
  t[231] = GetDirectBufferCapacity; t[232] = GetObjectRefType;

  vm_table[3] = DestroyJavaVM;
  vm_table[4] = AttachCurrentThread;
  vm_table[5] = DetachCurrentThread;
  vm_table[6] = GetEnv;
  vm_table[7] = AttachCurrentThread; // AttachCurrentThreadAsDaemon
  vm_ptr = &vm_obj;
  mkdir(g_savedir, 0755);
}
