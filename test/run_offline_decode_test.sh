#!/bin/bash
# Build & run the standalone offline decoder tests. macOS only, because of the
# platform codec it links. Run from the flutter_soloud repo root.
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

OUT="${TMPDIR:-/tmp}/offline_decode_test"
SO="src/soloud/src/audiosource/wav"

c++ -std=c++17 -O2 -Wall -fobjc-arc -Wno-unused-function -Wno-deprecated-declarations \
    -I src \
    -I src/soloud/include \
    -I src/native_decoder \
    -I "$XIPH/include" \
    -o "$OUT" \
    test/offline_decode_test.cpp \
    src/mixeroutput/offline_decoder.cpp \
    src/mixeroutput/offline_encoder.cpp \
    src/mixeroutput/mixer_output_encoder.cpp \
    src/mixeroutput/opus_output_encoder.cpp \
    src/mixeroutput/vorbis_output_encoder.cpp \
    src/mixeroutput/flac_output_encoder.cpp \
    src/mixeroutput/wav_output_encoder.cpp \
    src/mixeroutput/sinc_resampler.cpp \
    src/audiobuffer/stream_decoder.cpp \
    src/audiobuffer/aac_stream_decoder.cpp \
    src/audiobuffer/aac_stream_decoder_apple.mm \
    src/audiobuffer/flac_stream_decoder.cpp \
    src/audiobuffer/m4a_metadata.cpp \
    src/audiobuffer/mp3_stream_decoder.cpp \
    src/audiobuffer/ogg_flac_stream_decoder.cpp \
    src/audiobuffer/opus_stream_decoder.cpp \
    src/audiobuffer/vorbis_stream_decoder.cpp \
    src/audiobuffer/wav_stream_decoder.cpp \
    src/native_decoder/os_decoder_apple.mm \
    "$SO/dr_impl.cpp" \
    src/soloud/src/core/soloud_file.cpp \
    "$XIPH/macos/libopus.a" \
    "$XIPH/macos/libogg.a" \
    "$XIPH/macos/libvorbis.a" \
    "$XIPH/macos/libvorbisenc.a" \
    "$XIPH/macos/libvorbisfile.a" \
    "$XIPH/macos/libFLAC.a" \
    -framework AudioToolbox -framework CoreFoundation -framework Foundation -framework AVFoundation

"$OUT"
