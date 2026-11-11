#include <dlfcn.h>
#include "common.h"
#include "sdl.h"

#define DEF(ret, name, args) ret (*p##name) args;
SDL_FUNCS(DEF)
MIX_FUNCS(DEF)
#undef DEF
int g_have_mixer;

static void *open_first(const char *const *names) {
  for (; *names; names++) {
    void *h = dlopen(*names, RTLD_NOW | RTLD_GLOBAL);
    if (h) { LOG("loaded %s", *names); return h; }
  }
  return NULL;
}

int sdl_load(void) {
  static const char *const sdl_names[] = { "libSDL2-2.0.so.0", "libSDL2-2.0.so", "libSDL2.so", NULL };
  static const char *const mix_names[] = { "libSDL2_mixer-2.0.so.0", "libSDL2_mixer-2.0.so", "libSDL2_mixer.so", NULL };
  void *h = open_first(sdl_names);
  if (!h) { LOG("cannot load SDL2: %s", dlerror()); return -1; }
  int ok = 1;
#define LOAD(ret, name, args) \
  if (!(p##name = (ret (*) args)dlsym(h, #name))) { LOG("SDL2 lacks %s", #name); ok = 0; }
  SDL_FUNCS(LOAD)
#undef LOAD
  if (!ok) return -1;

  void *m = open_first(mix_names);
  if (!m) { LOG("no SDL2_mixer (%s): running without sound", dlerror()); return 0; }
  g_have_mixer = 1;
#define LOADM(ret, name, args) \
  if (!(p##name = (ret (*) args)dlsym(m, #name))) { LOG("SDL2_mixer lacks %s", #name); g_have_mixer = 0; }
  MIX_FUNCS(LOADM)
#undef LOADM
  return 0;
}
