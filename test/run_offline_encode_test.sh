#!/bin/bash
# Build & run the standalone offline encoder round-trip tests.
# Run from the flutter_soloud repo root.
#
# Needs the prebuilt xiph static libraries the build hook downloads into
# .dart_tool/flutter_soloud/xiph/prebuild, so build the example for macOS once
# first (cd example && flutter build macos).

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

XIPH=".dart_tool/flutter_soloud/xiph/prebuild"
if [ ! -d "$XIPH/macos" ]; then
    echo "Missing $XIPH/macos: run 'flutter build macos' in example/ first." >&2
    exit 1
fi

OUT="${TMPDIR:-/tmp}/offline_encode_test"

c++ -std=c++17 -O2 -Wall \
    -I src \
    -I "$XIPH/include" \
    -o "$OUT" \
    test/offline_encode_test.cpp \
    src/mixeroutput/offline_encoder.cpp \
    src/mixeroutput/mixer_output_encoder.cpp \
    src/mixeroutput/opus_output_encoder.cpp \
    src/mixeroutput/vorbis_output_encoder.cpp \
    src/mixeroutput/flac_output_encoder.cpp \
    src/mixeroutput/wav_output_encoder.cpp \
    src/mixeroutput/sinc_resampler.cpp \
    "$XIPH/macos/libopus.a" \
    "$XIPH/macos/libogg.a" \
    "$XIPH/macos/libvorbis.a" \
    "$XIPH/macos/libvorbisenc.a" \
    "$XIPH/macos/libFLAC.a"

"$OUT"
