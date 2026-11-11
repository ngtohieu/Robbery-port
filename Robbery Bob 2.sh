#!/bin/bash
# PORTMASTER: robberybob2.zip, Robbery Bob 2.sh
# Robbery Bob 2: Double Trouble (Android) via rb2loader.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/robberybob2"
if [ ! -d "$GAMEDIR" ]; then
  GAMEDIR="$(cd "$(dirname "$0")" && pwd)/robberybob2"
fi
cd "$GAMEDIR" || exit 1

# Straight to the file (no tee): the loader fsyncs it, so the log survives a freeze.
exec > "$GAMEDIR/log.txt" 2>&1
echo "== Robbery Bob 2 | cfw=${CFW_NAME:-unknown} device=${DEVICE_NAME:-unknown} ${DISPLAY_WIDTH}x${DISPLAY_HEIGHT} =="

# Record it if the launcher itself is told to stop (the loader logs its own signals).
for s in HUP INT TERM QUIT; do trap "echo \"launcher got SIG$s\"; sync" $s; done

APK=""
for f in "$GAMEDIR"/gamedata/*.apk "$GAMEDIR"/*.apk; do
  [ -f "$f" ] && { APK="$f"; break; }
done
if [ -z "$APK" ]; then
  echo "No APK found. Copy com.chillingo.robberybob2.apk into $GAMEDIR/gamedata/"
  if command -v pm_message >/dev/null 2>&1; then
    pm_message "Copy com.chillingo.robberybob2.apk into ports/robberybob2/gamedata/"
    sleep 5
  fi
  pm_finish 2>/dev/null
  exit 1
fi
echo "APK: $APK"
# Kernel messages up to now: they cover the previous run (out-of-memory kills etc.).
$ESUDO dmesg 2>/dev/null | tail -n 150 > "$GAMEDIR/dmesg.txt"

mkdir -p "$GAMEDIR/savedata"
export RB2_SAVE_DIR="$GAMEDIR/savedata"
# Controls, language and debug switches live in rb2.conf.
[ -f "$GAMEDIR/rb2.conf" ] && source "$GAMEDIR/rb2.conf"

export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export LD_LIBRARY_PATH="$GAMEDIR/libs:$LD_LIBRARY_PATH"

$ESUDO chmod +x "$GAMEDIR/rb2loader"

# gptokeyb only provides the hotkey exit; the loader reads the pad through SDL.
$GPTOKEYB "rb2loader" &
command -v pm_platform_helper >/dev/null 2>&1 && pm_platform_helper "$GAMEDIR/rb2loader"

./rb2loader "$APK"
echo "rb2loader exited with status $?"
[ -f crash.txt ] && echo "see crash.txt"
sync

$ESUDO kill -9 $(pidof gptokeyb) 2>/dev/null
pm_finish 2>/dev/null
printf "\033c" > /dev/tty0 2>/dev/null
