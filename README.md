# Robbery Bob 2: Double Trouble — PortMaster port

A PortMaster port of the Android game **Robbery Bob 2: Double Trouble**
(`com.chillingo.robberybob2`, version 1.8.0) for AArch64 Linux handhelds
running **muOS** or **dArkOS**.

The game is not a Unity title, so it does not use the NextOS Unity loader.
Instead it ships its own loader, `rb2loader`, which:

- loads the game's original Android libraries (`libc++_shared.so`,
  `librobberybob.so`) straight from the APK, with bionic → glibc shims;
- emulates the Java side of the game (JNI): saves, sound, settings;
- draws with SDL2 + OpenGL ES. If the device has no working GLES 1.1, it
  falls back to GLES 2.0 with a built-in fixed-function emulation layer;
- plays audio through SDL2_mixer (the ogg files are read from the APK).

Ads, in-app purchases, Google Play Games and YouTube are disabled.

Tested on: **dArkOSRE, RG351MP (640×480, Mali-G31)**.

## Requirements

- An AArch64 device with PortMaster installed (muOS, dArkOS or a similar CFW).
- glibc 2.18 or newer, SDL2 and SDL2_mixer. These come with the CFW and PortMaster.
- **Your own copy of the game APK.** It must contain the `arm64-v8a` libraries,
  for example `com.chillingo.robberybob2.android.gplay_1.8.0-416902_minAPI19(arm64-v8a,armeabi-v7a)(nodpi).apk`.
  The APK is not included.

## Installation

### 1. Install the port

**Option A: PortMaster autoinstall**

1. Copy `robberybob2.zip` into the `autoinstall` folder inside your PortMaster folder:
   - muOS: `/mnt/mmc/MUOS/PortMaster/autoinstall/`
   - dArkOS: `/opt/system/Tools/PortMaster/autoinstall/` (or `/roms/tools/PortMaster/autoinstall/`)
2. Open PortMaster. It installs the zip automatically.

**Option B: manual**

Extract `robberybob2.zip` into your ports folder, so that you end up with:

```
ports/
├── Robbery Bob 2.sh
└── robberybob2/
    ├── rb2loader
    ├── rb2.conf
    ├── gamedata/
    └── ...
```

The ports folder is:

- dArkOS: `/roms/ports/`
- muOS: `/mnt/mmc/ROMS/Ports/` (or the same path on the second SD card)

### 2. Add the game APK

Copy the APK into `ports/robberybob2/gamedata/`. Any file name ending in
`.apk` works. Do **not** extract it: the loader reads everything directly from it.

### 3. Run

Refresh the game list in EmulationStation (or restart the frontend), then start
**Robbery Bob 2** from the Ports section.

## Controls

| Button | Action |
|---|---|
| Left stick / D-pad | Move (virtual joystick) |
| X | Sprint (shoe button, hold) |
| Y | Use item (the "+" on the shoe button) |
| L1 | Gadgets (wrench button) |
| R1 | Fast forward |
| A | Tap at the cursor (menus) |
| Right stick | Move the cursor |
| B / Start | Android Back (pause / go back) |
| Select (press and release) | Switch the D-pad between joystick and cursor (for devices without analog sticks) |
| Select + L1 | Screenshot to `robberybob2/screenshots/` |
| Select + Start | Quit |

### Changing button positions

Every on-screen touch position is set in `robberybob2/rb2.conf`. Positions are
fractions of the screen: `0,0` is the top-left corner and `1,1` the bottom-right.

```sh
export RB2_JOY="0.177,0.754"   # virtual joystick centre
export RB2_JOY_RADIUS="0.12"   # joystick drag distance (fraction of screen height)
export RB2_TAP_X="0.79,0.82"   # sprint
export RB2_TAP_Y="0.86,0.72"   # use item "+"
export RB2_TAP_L1="0.34,0.14"  # gadgets
export RB2_TAP_R1="0.22,0.14"  # fast forward
export RB2_TAP_L2=""           # unused, set a position to enable
export RB2_TAP_R2=""
```

The defaults were measured on a 640×480 (4:3) screen. On other aspect ratios
the game's layout may shift. If it does, take a screenshot (Select + L1) and
adjust the values. Changes take effect on the next launch; no reinstall is needed.

## Other settings (`rb2.conf`)

| Setting | Meaning |
|---|---|
| `RB2_LANG` | Game language: `en`, `fr`, `de`, `es`, `it`, `id`, `hi`... (empty = `en`) |
| `RB2_GLES` | Graphics path. Empty: try GLES 1.1, then GLES 2.0 emulation. `1`: GLES 1.1 only. `2`: always use the emulation. |
| `RB2_VERBOSE` | `1` writes a detailed log (file access, JNI calls, touches, GL usage). Set `0` once everything works. |

## Saves

Progress is stored in `robberybob2/savedata/` (`userdefaults.gsd` and related
files). Back up this folder to keep your progress.

## Troubleshooting

When something goes wrong, check these files in `robberybob2/`:

- `log.txt`: the log of the last run. It is synced to the card every second,
  so it survives a freeze. Look for:
  - `fatal signal` (a crash, with a backtrace);
  - `watchdog` (the game hung);
  - `got signal` (the game was stopped from outside);
  - `WARNING`.
- `crash.txt`: written only when the game crashes or hangs.
- `dmesg.txt`: kernel messages saved at launch. It shows what happened during
  the previous run, for example out-of-memory kills.

Common problems:

| Problem | Fix |
|---|---|
| "No APK found" message | Put the `.apk` file in `robberybob2/gamedata/`. |
| `lib/arm64-v8a/... missing from the apk` | Your APK has no 64-bit libraries. Use an APK that contains `arm64-v8a`. |
| Black screen, music plays | Set `RB2_GLES="2"` in `rb2.conf`. |
| Buttons tap the wrong place | Adjust the `RB2_TAP_*` / `RB2_JOY` values (see above). |
| Game hangs | Wait about 60 s: the watchdog quits on its own and writes `crash.txt`. |

## Building the loader

The loader source is in `robberybob2/src/` (not included in the release zip).
You need Python with `pip install ziglang`:

```sh
cd robberybob2/src
sh build.sh              # builds ../rb2loader (aarch64, glibc >= 2.17)
cd ../..
python robberybob2/src/pack.py   # creates robberybob2.zip
```

## Credits

- Game: Level Eight / Chillingo. You need your own legal copy of the game.
- Port and loader: `rb2loader`, built in the style of the NextOS PortMaster ports.
