// rb2loader: runs the Android build of Robbery Bob 2 (librobberybob.so) on
// aarch64 glibc handhelds. Shared declarations.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define LOG(...) do { fprintf(stderr, "[rb2] " __VA_ARGS__); fputc('\n', stderr); } while (0)

extern int g_verbose;
#define VLOG(...) do { if (g_verbose) LOG(__VA_ARGS__); } while (0)

// ---- zip.c: read entries out of the APK -----------------------------------
int zip_open(const char *path);
// Returns a malloc'd buffer with the entry's contents, or NULL.
void *zip_read(const char *name, size_t *size);
int zip_exists(const char *name);

// ---- so_loader.c ------------------------------------------------------------
typedef struct so_module so_module;
typedef void *(*so_resolver)(const char *name);
so_module *so_load(const char *name, void *elf, size_t size, so_resolver resolve);
void so_run_init(so_module *m);
void *so_sym(so_module *m, const char *name);
void *so_find_global(const char *name); // defined symbol in any loaded module
int so_addr_info(uintptr_t addr, const char **name, uintptr_t *off);
struct dl_phdr_info;
int so_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data);

// ---- bionic.c ---------------------------------------------------------------
void *bionic_resolve(const char *name);
void bionic_init(void);
int bionic_thread_tids(int *out, int max); // tids of the threads the game started
void rb2_altstack(void);                   // alternate signal stack for this thread

// ---- jni.c ------------------------------------------------------------------
typedef struct jobj jobj;
void *jni_env(void);
void *jni_vm(void);
void jni_init(void);
jobj *jni_new_string(const char *s);
jobj *jni_new_int_array(int len);
int *jni_int_data(jobj *a);
jobj *jni_new_object(const char *cls);
const char *jni_string(jobj *s);

// ---- audio.c ----------------------------------------------------------------
int audio_init(void);
int audio_load_music(const char *path);
void audio_play_music(int loop);
void audio_stop_music(void);
int audio_music_playing(void);
void audio_music_volume(float v);
float audio_music_time(void);
void audio_set_music_time(float t);
void audio_pause_all(int pause);
int audio_load_sound(const char *path);
int audio_play_sound(int id, int loop, float vol, int use_vol);
void audio_stop_sound(int id);
void audio_unload_sound(int id);
int audio_sound_playing(int id);
void audio_sound_volume(int id, float v);
void audio_global_sound_volume(float v);
float audio_sound_duration(const char *path);

// ---- sdl.c: dlopen'd SDL2 / SDL2_mixer --------------------------------------
int sdl_load(void);

// ---- main.c -----------------------------------------------------------------
extern char g_gamedir[512];
extern char g_savedir[512];
extern char g_apk[512];
__attribute__((noreturn)) void rb2_quit(int code);
extern volatile int g_quit_requested;
void *gl_resolve(const char *name);

// ---- gles1emu.c: GLES 1.1 fixed function on a GLES 2 context ----------------
extern int g_gles1emu;
int gles1emu_init(void *(*get)(const char *));
void *gles1emu_proc(const char *name); // emulated entry point, or NULL
