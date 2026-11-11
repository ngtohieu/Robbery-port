// leSoundManager (SoundPool + MediaPlayer) on top of SDL2_mixer. Files come
// straight out of the APK's assets/.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "sdl.h"

#define MAX_SOUNDS 512
#define CHANNELS 32

typedef struct { char *path; Mix_Chunk *chunk; int channel; float volume; } sound;
static sound sounds[MAX_SOUNDS + 1]; // ids start at 1
static pthread_mutex_t alock = PTHREAD_MUTEX_INITIALIZER;
static int ready;
static float sfx_volume = 1.0f, music_volume = 1.0f;
static Mix_Music *music;
static void *music_buf;
static uint32_t music_start;
static int music_paused_by_app;

static void *load_asset(const char *path, size_t *size) {
  char name[512];
  if (!strncmp(path, "assets/", 7)) path += 7;
  while (*path == '/') path++;
  snprintf(name, sizeof name, "assets/%s", path);
  void *buf = zip_read(name, size);
  if (!buf) LOG("audio: %s not in apk", name);
  return buf;
}

int audio_init(void) {
  if (!g_have_mixer) return -1;
  pMix_Init(0x10 /* MIX_INIT_OGG */);
  if (pMix_OpenAudio(44100, 0x8010 /* AUDIO_S16SYS */, 2, 1024) < 0) {
    LOG("audio: Mix_OpenAudio failed: %s", pSDL_GetError());
    return -1;
  }
  pMix_AllocateChannels(CHANNELS);
  ready = 1;
  LOG("audio: SDL_mixer ready");
  return 0;
}

int audio_load_music(const char *path) {
  if (!ready || !path) return 0;
  size_t size;
  void *buf = load_asset(path, &size);
  if (!buf) return 0;
  pthread_mutex_lock(&alock);
  if (music) { pMix_HaltMusic(); pMix_FreeMusic(music); music = NULL; }
  free(music_buf);
  music_buf = buf;
  music = pMix_LoadMUS_RW(pSDL_RWFromConstMem(buf, (int)size), 1);
  pthread_mutex_unlock(&alock);
  if (!music) LOG("audio: cannot decode %s: %s", path, pSDL_GetError());
  VLOG("audio: music %s", path);
  return music != NULL;
}

void audio_play_music(int loop) {
  if (!ready || !music) return;
  pMix_VolumeMusic((int)(music_volume * 128));
  pMix_PlayMusic(music, loop ? -1 : 0);
  music_start = pSDL_GetTicks();
}
void audio_stop_music(void) { if (ready) pMix_HaltMusic(); }
int audio_music_playing(void) { return ready && pMix_PlayingMusic(); }
void audio_music_volume(float v) {
  music_volume = v < 0 ? 0 : v > 1 ? 1 : v;
  if (ready) pMix_VolumeMusic((int)(music_volume * 128));
}
float audio_music_time(void) { return ready && music ? (pSDL_GetTicks() - music_start) / 1000.0f : 0; }
void audio_set_music_time(float t) {
  if (!ready || !music) return;
  if (pMix_SetMusicPosition(t) == 0) music_start = pSDL_GetTicks() - (uint32_t)(t * 1000);
}

void audio_pause_all(int pause) {
  if (!ready) return;
  if (pause) {
    pMix_Pause(-1);
    if (pMix_PlayingMusic()) { pMix_PauseMusic(); music_paused_by_app = 1; }
  } else {
    pMix_Resume(-1);
    if (music_paused_by_app) { pMix_ResumeMusic(); music_paused_by_app = 0; }
  }
}

int audio_load_sound(const char *path) {
  if (!path) return 0;
  pthread_mutex_lock(&alock);
  for (int i = 1; i <= MAX_SOUNDS; i++)
    if (sounds[i].path && !strcmp(sounds[i].path, path)) { pthread_mutex_unlock(&alock); return i; }
  int id = 0;
  for (int i = 1; i <= MAX_SOUNDS; i++) if (!sounds[i].path) { id = i; break; }
  if (!id) { pthread_mutex_unlock(&alock); LOG("audio: sound table full"); return 0; }
  sounds[id].path = strdup(path);
  sounds[id].channel = -1;
  sounds[id].volume = 1.0f;
  pthread_mutex_unlock(&alock);
  if (ready) {
    size_t size;
    void *buf = load_asset(path, &size);
    if (buf) {
      Mix_Chunk *c = pMix_LoadWAV_RW(pSDL_RWFromConstMem(buf, (int)size), 1);
      free(buf);
      if (!c) LOG("audio: cannot decode %s: %s", path, pSDL_GetError());
      sounds[id].chunk = c;
    }
  }
  VLOG("audio: sound %d = %s", id, path);
  return id;
}

static sound *get(int id) { return id > 0 && id <= MAX_SOUNDS && sounds[id].path ? &sounds[id] : NULL; }

int audio_play_sound(int id, int loop, float vol, int use_vol) {
  sound *s = get(id);
  if (!s || !s->chunk || !ready) return id;
  if (use_vol) s->volume = vol;
  int ch = pMix_PlayChannelTimed(-1, s->chunk, loop, -1);
  if (ch >= 0) pMix_Volume(ch, (int)(s->volume * sfx_volume * 128));
  s->channel = ch;
  return id;
}

void audio_stop_sound(int id) {
  sound *s = get(id);
  if (s && ready && s->channel >= 0) { pMix_HaltChannel(s->channel); s->channel = -1; }
}

int audio_sound_playing(int id) {
  sound *s = get(id);
  return s && ready && s->channel >= 0 && pMix_Playing(s->channel);
}

void audio_sound_volume(int id, float v) {
  sound *s = get(id);
  if (!s) return;
  s->volume = v;
  if (ready && s->channel >= 0 && pMix_Playing(s->channel)) pMix_Volume(s->channel, (int)(v * sfx_volume * 128));
}

void audio_global_sound_volume(float v) { sfx_volume = v < 0 ? 0 : v > 1 ? 1 : v; }

void audio_unload_sound(int id) {
  pthread_mutex_lock(&alock);
  sound *s = get(id);
  if (s) {
    if (ready && s->channel >= 0) pMix_HaltChannel(s->channel);
    if (s->chunk) pMix_FreeChunk(s->chunk);
    free(s->path);
    memset(s, 0, sizeof *s);
  }
  pthread_mutex_unlock(&alock);
}

float audio_sound_duration(const char *path) {
  int id = audio_load_sound(path);
  sound *s = get(id);
  if (!s || !s->chunk) return 0;
  return s->chunk->alen / (44100.0f * 2 * 2);
}
