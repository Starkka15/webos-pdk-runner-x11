#!/bin/bash
# webOS PDK Game Runner (X11 edition)
# Runs ARM PDK game binaries via qemu-arm with direct X11 display.
# No display server, no SHM relay.
#
# Audio: SDL disk driver writes raw PCM to a FIFO; aplay reads it.
# Format auto-detected from games.cfg or defaults to 22050 Hz / S16_LE / stereo.
# Override: WEBOS_AUDIO_RATE, WEBOS_AUDIO_CHANNELS env vars before running.
#
# Usage: ./run.sh <app_id_or_path> [options]
#
# Options:
#   -scale N        Integer scale factor (e.g. -scale 2 for 960x640)
#   -fullscreen     Scale to fill the current screen (nearest-neighbour)
#   -rotate CW|CCW|180  Rotate rendered output
#   -res WxH        Set virtual screen resolution (e.g. -res 1024x768)
#
# Examples:
#   ./run.sh 4765                   # Classic Invaders, native 480x320
#   ./run.sh 4765 -scale 2          # 960x640 window
#   ./run.sh 4765 -fullscreen       # fill screen

RUNNER_DIR="$(cd "$(dirname "$0")" && pwd)"
GAMES_DIR="$HOME/Games/webos/apps"
LIBS_DIR="$RUNNER_DIR/libs"
# GCC 13 arm-linux-gnueabi cross-toolchain sysroot (has glibc 2.39)
SYSROOT="/usr/arm-linux-gnueabi"

GAME_ARG="$1"
shift

# Parse options
WEBOS_SCALE_ARG=""
WEBOS_FULLSCREEN_ARG=""
WEBOS_ROTATE_ARG=""
WEBOS_RES_W=""
WEBOS_RES_H=""
while [ $# -gt 0 ]; do
    case "$1" in
        -scale)
            shift
            WEBOS_SCALE_ARG="$1"
            ;;
        -fullscreen)
            WEBOS_FULLSCREEN_ARG="1"
            ;;
        -rotate)
            shift
            WEBOS_ROTATE_ARG="$1"
            ;;
        -res)
            shift
            WEBOS_RES_W="${1%%x*}"
            WEBOS_RES_H="${1##*x}"
            ;;
        *)
            ;;
    esac
    shift
done

if [ -z "$GAME_ARG" ]; then
    echo "Usage: $0 <app_id_or_path>"
    echo ""
    echo "Available games:"
    for d in "$GAMES_DIR"/*/; do
        id=$(basename "$d")
        appdir=$(find "$d" -name appinfo.json -exec dirname {} \; 2>/dev/null | head -1)
        if [ -n "$appdir" ]; then
            name=$(grep -o '"title"[[:space:]]*:[[:space:]]*"[^"]*"' "$appdir/appinfo.json" 2>/dev/null | head -1 | sed 's/.*"\([^"]*\)"/\1/')
            echo "  $id  $name"
        fi
    done
    exit 1
fi

# Resolve game directory
if [ -d "$GAME_ARG" ]; then
    GAME_DIR="$GAME_ARG"
elif [ -d "$GAMES_DIR/$GAME_ARG" ]; then
    GAME_DIR=$(find "$GAMES_DIR/$GAME_ARG" -name appinfo.json -exec dirname {} \; 2>/dev/null | head -1)
else
    echo "Error: Game not found: $GAME_ARG"
    exit 1
fi

# Find ARM ELF binary
GAME_BIN=""
MAIN_FIELD=$(grep -o '"main"[[:space:]]*:[[:space:]]*"[^"]*"' "$GAME_DIR/appinfo.json" 2>/dev/null | sed 's/.*"\([^"]*\)"/\1/')
if [ -n "$MAIN_FIELD" ] && [ -f "$GAME_DIR/$MAIN_FIELD" ]; then
    if file "$GAME_DIR/$MAIN_FIELD" 2>/dev/null | grep -q "ELF.*ARM"; then
        GAME_BIN="$GAME_DIR/$MAIN_FIELD"
    fi
fi
if [ -z "$GAME_BIN" ]; then
    for f in "$GAME_DIR"/* "$GAME_DIR"/*/*; do
        if [ -f "$f" ] && file "$f" 2>/dev/null | grep -q "ELF.*ARM"; then
            GAME_BIN="$f"
            break
        fi
    done
fi
if [ -z "$GAME_BIN" ]; then
    echo "Error: No ARM binary found in $GAME_DIR"
    exit 1
fi

chmod +x "$GAME_BIN" 2>/dev/null
GAME_REL="${GAME_BIN#$GAME_DIR/}"

echo "Game: $(basename "$GAME_BIN")"
echo "Dir:  $GAME_DIR"

cd "$GAME_DIR"

DISPLAY="${DISPLAY:-:0}"

# Audio via FIFO: SDL disk driver → FIFO → audio_relay → ALSA
# SDL_mixer defaults to 22050 Hz stereo S16LE; most webOS games match this.
AUDIO_RATE="${WEBOS_AUDIO_RATE:-22050}"
AUDIO_CHANNELS="${WEBOS_AUDIO_CHANNELS:-2}"
AUDIO_FIFO="/tmp/webos_pdk_audio_$$"
mkfifo "$AUDIO_FIFO"

AUDIO_RELAY_PID=""
QEMU_PID=""
_cleanup_done=0
cleanup() {
    [ "$_cleanup_done" = "1" ] && return
    _cleanup_done=1
    [ -n "$QEMU_PID" ]         && kill "$QEMU_PID"         2>/dev/null
    [ -n "$GL_RELAY_PID" ]     && kill "$GL_RELAY_PID"     2>/dev/null
    [ -n "$AUDIO_RELAY_PID" ]  && kill "$AUDIO_RELAY_PID"  2>/dev/null
    wait 2>/dev/null
    rm -f "$AUDIO_FIFO"
}
trap cleanup EXIT INT TERM

# Start audio_relay reading from FIFO.
# audio_relay uses a tight ALSA buffer (~93ms) and drops audio that drifts
# more than 120ms ahead of the wall clock, keeping game audio in sync.
"$RUNNER_DIR/audio_relay" "$AUDIO_FIFO" "$AUDIO_RATE" "$AUDIO_CHANNELS" &
AUDIO_RELAY_PID=$!

# Detect primary monitor: W H X Y
_detect_primary() {
    local LINE
    LINE=$(xrandr 2>/dev/null | grep " connected primary" | head -1)
    [ -z "$LINE" ] && LINE=$(xrandr 2>/dev/null | grep " connected" | head -1)
    local RES
    RES=$(echo "$LINE" | grep -oP '\d+x\d+\+\d+\+\d+' | head -1)
    [ -z "$RES" ] && return
    echo "$RES"
}

PRIMARY_RES=$(_detect_primary)
PRIMARY_W=$(echo "$PRIMARY_RES" | grep -oP '^\d+')
PRIMARY_H=$(echo "$PRIMARY_RES" | grep -oP 'x\K\d+')
PRIMARY_X=$(echo "$PRIMARY_RES" | grep -oP '\+\K\d+' | head -1)
PRIMARY_Y=$(echo "$PRIMARY_RES" | grep -oP '\+\K\d+' | tail -1)

# Build scale/position env args for qemu
SCALE_ENV_ARGS=()
if [ -n "$WEBOS_RES_W" ]; then
    SCALE_ENV_ARGS+=(-E "WEBOS_SCREEN_W=$WEBOS_RES_W" -E "WEBOS_SCREEN_H=$WEBOS_RES_H")
    echo "Resolution: ${WEBOS_RES_W}x${WEBOS_RES_H}"
elif [ -n "$WEBOS_FULLSCREEN_ARG" ] && [ -n "$PRIMARY_W" ]; then
    # Pass screen dimensions — SDL computes the correct scale against the actual game resolution
    SCALE_ENV_ARGS+=(-E "WEBOS_SCREEN_W=$PRIMARY_W" -E "WEBOS_SCREEN_H=$PRIMARY_H")
    echo "Screen: ${PRIMARY_W}x${PRIMARY_H} at +${PRIMARY_X}+${PRIMARY_Y} (SDL will compute scale)"
elif [ -n "$WEBOS_SCALE_ARG" ]; then
    SCALE_ENV_ARGS+=(-E "WEBOS_SCALE=$WEBOS_SCALE_ARG")
    echo "Scale: ${WEBOS_SCALE_ARG}x"
fi
# Always pass window position when we have a target monitor
if [ -n "$PRIMARY_X" ] && { [ -n "$WEBOS_FULLSCREEN_ARG" ] || [ -n "$WEBOS_SCALE_ARG" ]; }; then
    SCALE_ENV_ARGS+=(-E "WEBOS_WIN_X=$PRIMARY_X" -E "WEBOS_WIN_Y=$PRIMARY_Y")
    echo "Window position: +${PRIMARY_X}+${PRIMARY_Y}"
fi

# Detect GLES game: links against libGLES_CM
GL_RELAY_PID=""
IS_GLES=0
if readelf -d "$GAME_BIN" 2>/dev/null | grep -q "libGLES_CM"; then
    IS_GLES=1
fi

if [ "$IS_GLES" = "1" ]; then
    echo "GLES game detected — launching gl_relay"

    # Build gl_relay window size env for relay process
    GL_RELAY_ENV=()
    if [ -n "$WEBOS_RES_W" ]; then
        GL_RELAY_ENV+=(WEBOS_WIN_W="$WEBOS_RES_W" WEBOS_WIN_H="$WEBOS_RES_H")
    elif [ -n "$WEBOS_FULLSCREEN_ARG" ] && [ -n "$PRIMARY_W" ]; then
        GL_RELAY_ENV+=(WEBOS_SCREEN_W="$PRIMARY_W" WEBOS_SCREEN_H="$PRIMARY_H")
    elif [ -n "$WEBOS_SCALE_ARG" ]; then
        # For explicit scale we pass pixel size to relay (game res unknown yet,
        # use 480x320 baseline scaled — relay will use glViewport for mouse scaling)
        GL_RELAY_ENV+=(WEBOS_WIN_W="$(( 480 * WEBOS_SCALE_ARG ))" WEBOS_WIN_H="$(( 320 * WEBOS_SCALE_ARG ))")
    fi
    if [ -n "$PRIMARY_X" ] && { [ -n "$WEBOS_FULLSCREEN_ARG" ] || [ -n "$WEBOS_SCALE_ARG" ]; }; then
        GL_RELAY_ENV+=(WEBOS_WIN_X="$PRIMARY_X" WEBOS_WIN_Y="$PRIMARY_Y")
    fi
    if [ -n "$WEBOS_ROTATE_ARG" ]; then
        GL_RELAY_ENV+=(WEBOS_ROTATE="$WEBOS_ROTATE_ARG")
    fi

    env DISPLAY="$DISPLAY" \
        XAUTHORITY="${XAUTHORITY:-/run/user/$(id -u)/gdm/Xauthority}" \
        "${GL_RELAY_ENV[@]}" \
        "$RUNNER_DIR/gl_relay" &
    GL_RELAY_PID=$!

    # Give relay time to create SHM before the game tries to open it
    sleep 0.5
fi

if [ "$IS_GLES" = "1" ]; then
    VIDEODRIVER=dummy
else
    VIDEODRIVER=x11
fi

qemu-arm -L "$SYSROOT" \
  -s 16M \
  -E LD_LIBRARY_PATH="$LIBS_DIR" \
  -E DISPLAY="$DISPLAY" \
  -E XAUTHORITY="${XAUTHORITY:-/run/user/$(id -u)/gdm/Xauthority}" \
  -E SDL_VIDEODRIVER="$VIDEODRIVER" \
  -E SDL_AUDIODRIVER=disk \
  -E SDL_DISKAUDIOFILE="$AUDIO_FIFO" \
  -E SDL_DISKAUDIODELAY=46 \
  -E LANG=C \
  -E LD_PRELOAD=font_redirect.so \
  -E WEBOS_FONT_PATH="$RUNNER_DIR/fonts" \
  "${SCALE_ENV_ARGS[@]}" \
  ./$GAME_REL &
QEMU_PID=$!

wait "$QEMU_PID"

# Kill relay after game exits
[ -n "$GL_RELAY_PID" ] && kill "$GL_RELAY_PID" 2>/dev/null

cleanup
