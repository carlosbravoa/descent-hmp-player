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
# newer ABI. Strip any path that ships a foreign libstdc++ from the search path
# so the real system libraries are used for this build.
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

# ── 1. Fetch libADLMIDI if not present ───────────────────────────────────────
if [ ! -d "$LIBADLMIDI_DIR/.git" ]; then
    echo ">>> Cloning libADLMIDI..."
    git clone --depth=1 https://github.com/Wohlstand/libADLMIDI.git "$LIBADLMIDI_DIR"
else
    echo ">>> libADLMIDI already cloned (run 'git pull' inside $LIBADLMIDI_DIR to update)"
fi

# ── 2. Build libADLMIDI (skip if 'fast' argument given and lib already exists) ─
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

# ── 3. Compile hmpplay_opl3 ───────────────────────────────────────────────────
echo ">>> Compiling hmpplay_opl3..."
g++ -std=c++17 -O2 -Wall -Wextra \
    -I"$INSTALL_DIR/include" \
    "$SCRIPT_DIR/hmpplay_opl3.cpp" \
    -L"$INSTALL_DIR/lib" \
    -lADLMIDI \
    -o "$SCRIPT_DIR/hmpplay_opl3"

echo ""
echo ">>> Done: $SCRIPT_DIR/hmpplay_opl3"
echo ""
echo "Usage:"
echo "  ./hmpplay_opl3 ./music/"
echo "  ./hmpplay_opl3 --bank int -l ./music/"
echo "  ./hmpplay_opl3 --bank d2 ./music/"
