#!/usr/bin/env bash
# build.sh — builds libADLMIDI with RetroWave serial support,
#            then compiles hmpplay_opl3.
#
# Run once:  ./build.sh
# Rebuild after source changes only:  ./build.sh fast
#
# Produces:  ./hmpplay_opl3

set -euo pipefail

# ── Sanitize LD_LIBRARY_PATH ──────────────────────────────────────────────────
# Some third-party IDEs (e.g. Gowin) prepend their own lib directory containing
# an older libstdc++.so.6, which breaks cmake and g++ when the system needs a
# newer ABI. Strip any path that ships a foreign libstdc++ from the search path.
if [ -n "${LD_LIBRARY_PATH:-}" ]; then
    CLEAN_LLP=""
    IFS=: read -ra LLP_PARTS <<< "$LD_LIBRARY_PATH"
    for p in "${LLP_PARTS[@]}"; do
        if [ -f "$p/libstdc++.so.6" ] && \
           [[ "$p" != /usr/lib* ]] && \
           [[ "$p" != /lib* ]]; then
            echo ">>> Dropping '$p' from LD_LIBRARY_PATH (foreign libstdc++ detected)"
        else
            CLEAN_LLP="${CLEAN_LLP:+$CLEAN_LLP:}$p"
        fi
    done
    export LD_LIBRARY_PATH="$CLEAN_LLP"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIBADLMIDI_DIR="$SCRIPT_DIR/libADLMIDI"
BUILD_DIR="$SCRIPT_DIR/build/libadlmidi"
INSTALL_DIR="$SCRIPT_DIR/build/install"

# ── 1. Fetch libADLMIDI (git submodule) ──────────────────────────────────────
# libADLMIDI is a pinned submodule. Initialise it if the checkout is empty
# (i.e. the repo was cloned without --recurse-submodules).
if [ ! -f "$LIBADLMIDI_DIR/CMakeLists.txt" ]; then
    if [ -f "$SCRIPT_DIR/.gitmodules" ] && git -C "$SCRIPT_DIR" rev-parse --git-dir >/dev/null 2>&1; then
        echo ">>> Initialising libADLMIDI submodule..."
        git -C "$SCRIPT_DIR" submodule update --init --recursive libADLMIDI
    else
        echo ">>> Cloning libADLMIDI..."
        git clone https://github.com/Wohlstand/libADLMIDI.git "$LIBADLMIDI_DIR"
    fi
else
    echo ">>> libADLMIDI present"
fi

# ── 2. Patch baud rate support ────────────────────────────────────────────────
# libADLMIDI's ChipSerialPort::baud2enum() caps at B230400, which corrupts
# data to the RetroWave OPL3 Express that requires exactly 2,000,000 baud.
# B2000000 is a standard Linux termios constant — we just need to expose it.
SERIAL_MISC="$LIBADLMIDI_DIR/src/chips/opl_serial_misc.h"
python3 - "$SERIAL_MISC" << 'PYEOF'
import re, sys

path = sys.argv[1]
with open(path, 'r') as f:
    src = f.read()

if 'B2000000' in src:
    print(">>> Baud rate patch already applied")
    sys.exit(0)

patched = re.sub(
    r'(        else if\(baud <= 115200\)\n            return B115200;\n        else\n            return B230400;)',
    ('        else if(baud <= 115200)\n            return B115200;\n'
     '        else if(baud <= 230400)\n            return B230400;\n'
     '        else if(baud <= 1000000)\n            return B1000000;\n'
     '        else if(baud <= 1152000)\n            return B1152000;\n'
     '        else if(baud <= 1500000)\n            return B1500000;\n'
     '        else if(baud <= 2000000)\n            return B2000000;\n'
     '        else\n            return B2000000;'),
    src,
    count=1  # only the Linux section, not the Win32 one
)

if patched != src:
    with open(path, 'w') as f:
        f.write(patched)
    print(">>> Baud rate patch applied (added B2000000 support)")
else:
    print(">>> WARNING: baud rate patch target not found — check opl_serial_misc.h manually")
PYEOF

# ── 2b. Patch OPL register-write tap ──────────────────────────────────────────
# The --gui visualizer mirrors the OPL register stream into a software OPL3 tee.
# Expose a tap callback in the serial backend's writeReg() (idempotent).
SERIAL_PORT="$LIBADLMIDI_DIR/src/chips/opl_serial_port.cpp"
python3 - "$SERIAL_PORT" << 'PYEOF'
import sys
path = sys.argv[1]
src = open(path).read()
if 'g_retrowave_opl_tap' in src:
    print(">>> OPL tap patch already applied"); sys.exit(0)
glob = ('// retrowave visualizer tap (added by build.sh)\n'
        'extern "C" { void (*g_retrowave_opl_tap)(uint16_t addr, uint8_t data) = NULL; }\n\n'
        'OPL_SerialPort::OPL_SerialPort()')
src2 = src.replace('OPL_SerialPort::OPL_SerialPort()', glob, 1)
call = ('void OPL_SerialPort::writeReg(uint16_t addr, uint8_t data)\n{\n'
        '    if(g_retrowave_opl_tap)\n        g_retrowave_opl_tap(addr, data);\n')
src2 = src2.replace('void OPL_SerialPort::writeReg(uint16_t addr, uint8_t data)\n{\n', call, 1)
if src2 != src:
    open(path, 'w').write(src2); print(">>> OPL tap patch applied")
else:
    print(">>> WARNING: OPL tap patch targets not found — check opl_serial_port.cpp")
PYEOF

# ── 3. Build libADLMIDI (skip if 'fast' argument given and lib already exists) ─
LIBFILE="$INSTALL_DIR/lib/libADLMIDI.a"

if [ "${1:-}" = "fast" ] && [ -f "$LIBFILE" ]; then
    echo ">>> Skipping libADLMIDI build (fast mode, library exists)"
else
    echo ">>> Building libADLMIDI..."
    cmake -B "$BUILD_DIR" -S "$LIBADLMIDI_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
        -DlibADLMIDI_STATIC=ON \
        -DlibADLMIDI_SHARED=OFF \
        -DUSE_HW_SERIAL=ON \
        -DWITH_MIDI_SEQUENCER=ON \
        -DWITH_EMBEDDED_BANKS=ON \
        -DUSE_DOSBOX_EMULATOR=ON \
        -DUSE_NUKED_EMULATOR=OFF \
        -DUSE_OPAL_EMULATOR=OFF \
        -DUSE_JAVA_EMULATOR=OFF \
        -DWITH_MIDIPLAY=OFF \
        -DWITH_GENADLDATA=OFF \
        -DWITH_OLD_UTILS=OFF \
        2>&1 | grep -v "^--"

    cmake --build "$BUILD_DIR" --parallel "$(nproc)"
    cmake --install "$BUILD_DIR"
    echo ">>> libADLMIDI installed to $INSTALL_DIR"
fi

# ── 4. Compile hmpplay_opl3 ──────────────────────────────────────────────────
# The terminal player needs only cmake + a C++ compiler. The optional --gui
# visualizer needs SDL2; it is compiled in only if SDL2 is detected, so a
# terminal-only user needs no extra dependencies.
echo ">>> Compiling hmpplay_opl3..."

SDL_CFLAGS="$(sdl2-config --cflags 2>/dev/null || pkg-config --cflags sdl2 2>/dev/null || true)"
SDL_LIBS="$(sdl2-config --libs 2>/dev/null || pkg-config --libs sdl2 2>/dev/null || true)"

if [ -n "$SDL_LIBS" ]; then
    echo ">>> SDL2 found — building with --gui visualizer"
    # Software OPL3 "tee" for the visualizer (DOSBox emulator, OPL3 mode, C).
    gcc -std=gnu11 -O2 -DOPLTYPE_IS_OPL3 \
        -c "$SCRIPT_DIR/viz/opl.c" -o "$SCRIPT_DIR/build/opl_tee.o"

    g++ -std=c++17 -O2 -Wall -Wextra -DHAVE_GUI \
        -I"$INSTALL_DIR/include" -I"$SCRIPT_DIR/viz" $SDL_CFLAGS \
        "$SCRIPT_DIR/hmpplay_opl3.cpp" \
        "$SCRIPT_DIR/viz/hmpviz.cpp" \
        "$SCRIPT_DIR/build/opl_tee.o" \
        -L"$INSTALL_DIR/lib" \
        -lADLMIDI $SDL_LIBS \
        -o "$SCRIPT_DIR/hmpplay_opl3"
else
    echo ">>> SDL2 not found — building terminal-only (no --gui). "
    echo "    Install libsdl2-dev and rebuild to enable the visualizer."
    g++ -std=c++17 -O2 -Wall -Wextra \
        -I"$INSTALL_DIR/include" \
        "$SCRIPT_DIR/hmpplay_opl3.cpp" \
        -L"$INSTALL_DIR/lib" \
        -lADLMIDI \
        -o "$SCRIPT_DIR/hmpplay_opl3"
fi

echo ""
echo ">>> Done: $SCRIPT_DIR/hmpplay_opl3"
echo ""
echo "Usage:"
echo "  ./hmpplay_opl3 ./music/          # per-song banks from descent.sng"
echo "  ./hmpplay_opl3 -l ./music/       # loop the whole soundtrack"
echo "  ./hmpplay_opl3 --bank int ./music/   # force one bank (A/B testing)"
