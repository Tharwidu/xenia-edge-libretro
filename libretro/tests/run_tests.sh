#!/usr/bin/env bash
#
# Xenia Edge - libretro audio mixer tests
#
# Builds and runs the mixer regression tests under two sanitizer
# configurations. The code under test is EXTRACTED from the shipped sources by
# sed rather than duplicated here, so these tests cannot silently drift away
# from what actually ships - if the markers move or the code stops being
# self-contained, this script fails loudly instead of testing a stale copy.
#
# No Xenia build is required; this compiles in about a second.
#
# Usage: libretro/tests/run_tests.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/.."
GEN="$HERE/gen"

CXX="${CXX:-g++}"
BEGIN='==== BEGIN MIXER CORE'
END='==== END MIXER CORE'

rm -rf "$GEN"
mkdir -p "$GEN"

extract() {
    local in="$1" out="$2"
    sed -n "/$BEGIN/,/$END/p" "$in" > "$out"
    if [ ! -s "$out" ]; then
        echo "ERROR: no mixer-core region found in $in" >&2
        echo "       expected markers '$BEGIN' ... '$END'" >&2
        exit 1
    fi
}

extract "$SRC/libretro_audio_driver.h"  "$GEN/mixer_core.h.inc"
extract "$SRC/libretro_audio_driver.cc" "$GEN/mixer_core.cc.inc"

# The extracted region must not drag in Xenia headers, or it is no longer
# testable in isolation and the extraction has stopped being meaningful.
if grep -qE '^\s*#\s*include\s*[<"]xenia/' "$GEN"/mixer_core.*.inc; then
    echo "ERROR: the mixer core region now includes Xenia headers." >&2
    echo "       Keep that region dependency-free, or these tests cannot" >&2
    echo "       build it standalone. Offending lines:" >&2
    grep -nE '^\s*#\s*include\s*[<"]xenia/' "$GEN"/mixer_core.*.inc >&2
    exit 1
fi

echo "extracted $(wc -l < "$GEN/mixer_core.h.inc") + \
$(wc -l < "$GEN/mixer_core.cc.inc") lines from the shipped sources"

COMMON=(-std=c++17 -I"$HERE" -Wall -Wextra -Wno-unused-parameter -g -O1 -pthread)
FAILED=0

build_and_run() {
    local cc="$1" name="$2"; shift 2
    local bin="$GEN/test_$name"
    echo
    echo "=== $name ($cc) ==="
    if ! "$cc" "${COMMON[@]}" "$@" \
            "$HERE/test_libretro_audio_mixer.cc" -o "$bin"; then
        echo "BUILD FAILED ($name)" >&2
        FAILED=1
        return
    fi
    if ! "$bin"; then
        echo "TESTS FAILED ($name)" >&2
        FAILED=1
    fi
}

# Pick a compiler whose sanitizer runtime is actually installed. GCC on Fedora
# needs the separate libasan/libtsan packages and fails at link without them,
# while clang ships compiler-rt in the box - so prefer whichever links.
sanitizer_cc() {
    local probe="$GEN/.probe.cc" out="$GEN/.probe"
    echo 'int main(){return 0;}' > "$probe"
    for cc in "$CXX" clang++; do
        command -v "$cc" >/dev/null 2>&1 || continue
        if "$cc" -fsanitize=address "$probe" -o "$out" >/dev/null 2>&1; then
            echo "$cc"
            return 0
        fi
    done
    return 1
}

# Plain build first: if the logic is wrong, say so before the sanitizer noise.
build_and_run "$CXX" "plain"

if SAN_CC="$(sanitizer_cc)"; then
    # ASan+UBSan catches the overflow/underflow class of bug; TSan is the one
    # that matters for the SPSC ring, and is what caught the drop-oldest data
    # race on the payload that led to drop-newest.
    build_and_run "$SAN_CC" "asan_ubsan" -fsanitize=address,undefined -fno-omit-frame-pointer
    build_and_run "$SAN_CC" "tsan"       -fsanitize=thread            -fno-omit-frame-pointer
else
    echo
    echo "WARNING: no compiler with a working sanitizer runtime was found." >&2
    echo "         Ran the plain build only - the SPSC ring's thread safety" >&2
    echo "         is NOT verified by this run. Install libasan/libtsan (GCC)" >&2
    echo "         or clang, then re-run." >&2
    FAILED=1
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "RESULT: FAILURES"
    exit 1
fi
echo "RESULT: all configurations passed"
