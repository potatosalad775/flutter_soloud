#!/bin/bash
# Build & run the standalone Opus seek-accuracy tests.
# Run from the flutter_soloud repo root.
#
# Needs the prebuilt xiph static libraries under macos/flutter_soloud/libs.

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

OUT="${TMPDIR:-/tmp}/opus_seek_test"

c++ -std=c++17 -O2 -Wall \
    -I src \
    -I src/soloud/include \
    -I macos/flutter_soloud/include \
    -o "$OUT" \
    test/opus_seek_test.cpp \
    src/soloud/src/audiosource/wav/mb_ogg.cpp \
    src/soloud/src/core/soloud_file.cpp \
    src/mixeroutput/offline_encoder.cpp \
    src/mixeroutput/mixer_output_encoder.cpp \
    src/mixeroutput/opus_output_encoder.cpp \
    src/mixeroutput/vorbis_output_encoder.cpp \
    src/mixeroutput/flac_output_encoder.cpp \
    src/mixeroutput/wav_output_encoder.cpp \
    src/mixeroutput/sinc_resampler.cpp \
    macos/flutter_soloud/libs/libopus.a \
    macos/flutter_soloud/libs/libogg.a \
    macos/flutter_soloud/libs/libvorbis.a \
    macos/flutter_soloud/libs/libvorbisenc.a \
    macos/flutter_soloud/libs/libvorbisfile.a \
    macos/flutter_soloud/libs/libFLAC.a

"$OUT"
