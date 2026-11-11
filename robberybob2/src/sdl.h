// Minimal SDL2 / SDL2_mixer API, loaded with dlopen at runtime so the loader
// builds without the target's SDL headers or libraries.
#pragma once
#include <stdint.h>

#define SDL_INIT_AUDIO          0x00000010u
#define SDL_INIT_VIDEO          0x00000020u
#define SDL_INIT_JOYSTICK       0x00000200u
#define SDL_INIT_GAMECONTROLLER 0x00002000u
#define SDL_INIT_EVENTS         0x00004000u

#define SDL_WINDOW_FULLSCREEN   0x00000001u
#define SDL_WINDOW_OPENGL       0x00000002u
#define SDL_WINDOW_SHOWN        0x00000004u
#define SDL_WINDOWPOS_UNDEFINED 0x1FFF0000u

enum {
  SDL_GL_RED_SIZE = 0, SDL_GL_GREEN_SIZE = 1, SDL_GL_BLUE_SIZE = 2,
  SDL_GL_ALPHA_SIZE = 3, SDL_GL_DOUBLEBUFFER = 5, SDL_GL_DEPTH_SIZE = 6,
  SDL_GL_STENCIL_SIZE = 7, SDL_GL_CONTEXT_MAJOR_VERSION = 17,
  SDL_GL_CONTEXT_MINOR_VERSION = 18, SDL_GL_CONTEXT_PROFILE_MASK = 21,
};
#define SDL_GL_CONTEXT_PROFILE_ES 0x0004

#define SDL_QUIT                    0x100
#define SDL_KEYDOWN                 0x300
#define SDL_KEYUP                   0x301
#define SDL_CONTROLLERAXISMOTION    0x650
#define SDL_CONTROLLERBUTTONDOWN    0x651
#define SDL_CONTROLLERBUTTONUP      0x652
#define SDL_CONTROLLERDEVICEADDED   0x653
#define SDL_CONTROLLERDEVICEREMOVED 0x654

enum {
  SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X,
  SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_GUIDE,
  SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_LEFTSTICK,
  SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
  SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_DPAD_UP,
  SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
  SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_MAX = 21
};
enum {
  SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, SDL_CONTROLLER_AXIS_RIGHTX,
  SDL_CONTROLLER_AXIS_RIGHTY, SDL_CONTROLLER_AXIS_TRIGGERLEFT,
  SDL_CONTROLLER_AXIS_TRIGGERRIGHT, SDL_CONTROLLER_AXIS_MAX
};

typedef struct { uint32_t type, timestamp; int32_t which; uint8_t axis, p1, p2, p3; int16_t value; uint16_t p4; } SDL_ControllerAxisEvent;
typedef struct { uint32_t type, timestamp; int32_t which; uint8_t button, state, p1, p2; } SDL_ControllerButtonEvent;
typedef struct { uint32_t type, timestamp; int32_t which; } SDL_ControllerDeviceEvent;
typedef union {
  uint32_t type;
  SDL_ControllerAxisEvent caxis;
  SDL_ControllerButtonEvent cbutton;
  SDL_ControllerDeviceEvent cdevice;
  uint8_t padding[56];
} SDL_Event;

typedef struct { uint32_t format; int w, h, refresh_rate; void *driverdata; } SDL_DisplayMode;
typedef struct SDL_RWops SDL_RWops;
typedef struct { int allocated; uint8_t *abuf; uint32_t alen; uint8_t volume; } Mix_Chunk;
typedef struct Mix_Music Mix_Music;

#define SDL_FUNCS(X) \
  X(int, SDL_Init, (uint32_t)) \
  X(void, SDL_Quit, (void)) \
  X(const char *, SDL_GetError, (void)) \
  X(int, SDL_GL_SetAttribute, (int, int)) \
  X(void *, SDL_CreateWindow, (const char *, int, int, int, int, uint32_t)) \
  X(void, SDL_GetWindowSize, (void *, int *, int *)) \
  X(int, SDL_GetCurrentDisplayMode, (int, SDL_DisplayMode *)) \
  X(void *, SDL_GL_CreateContext, (void *)) \
  X(void, SDL_GL_DeleteContext, (void *)) \
  X(void, SDL_DestroyWindow, (void *)) \
  X(const char *, SDL_GetCurrentVideoDriver, (void)) \
  X(int, SDL_GL_MakeCurrent, (void *, void *)) \
  X(void, SDL_GL_SwapWindow, (void *)) \
  X(int, SDL_GL_SetSwapInterval, (int)) \
  X(void *, SDL_GL_GetProcAddress, (const char *)) \
  X(void, SDL_GL_GetDrawableSize, (void *, int *, int *)) \
  X(int, SDL_ShowCursor, (int)) \
  X(int, SDL_PollEvent, (SDL_Event *)) \
  X(int, SDL_NumJoysticks, (void)) \
  X(int, SDL_IsGameController, (int)) \
  X(void *, SDL_GameControllerOpen, (int)) \
  X(const char *, SDL_GameControllerName, (void *)) \
  X(uint32_t, SDL_GetTicks, (void)) \
  X(void, SDL_Delay, (uint32_t)) \
  X(SDL_RWops *, SDL_RWFromConstMem, (const void *, int))

#define MIX_FUNCS(X) \
  X(int, Mix_Init, (int)) \
  X(int, Mix_OpenAudio, (int, uint16_t, int, int)) \
  X(int, Mix_AllocateChannels, (int)) \
  X(Mix_Chunk *, Mix_LoadWAV_RW, (SDL_RWops *, int)) \
  X(Mix_Music *, Mix_LoadMUS_RW, (SDL_RWops *, int)) \
  X(void, Mix_FreeChunk, (Mix_Chunk *)) \
  X(void, Mix_FreeMusic, (Mix_Music *)) \
  X(int, Mix_PlayChannelTimed, (int, Mix_Chunk *, int, int)) \
  X(int, Mix_HaltChannel, (int)) \
  X(int, Mix_Playing, (int)) \
  X(int, Mix_Volume, (int, int)) \
  X(int, Mix_PlayMusic, (Mix_Music *, int)) \
  X(int, Mix_HaltMusic, (void)) \
  X(int, Mix_PlayingMusic, (void)) \
  X(int, Mix_VolumeMusic, (int)) \
  X(void, Mix_PauseMusic, (void)) \
  X(void, Mix_ResumeMusic, (void)) \
  X(void, Mix_Pause, (int)) \
  X(void, Mix_Resume, (int)) \
  X(int, Mix_SetMusicPosition, (double))

#define DECL(ret, name, args) extern ret (*p##name) args;
SDL_FUNCS(DECL)
MIX_FUNCS(DECL)
#undef DECL
extern int g_have_mixer;
