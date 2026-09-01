#!/bin/bash
# Build & run the standalone windowed-sinc resampler tests.
# Run from the flutter_soloud repo root.

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

OUT="${TMPDIR:-/tmp}/sinc_resampler_test"

c++ -std=c++17 -O2 -Wall \
    -I src \
    -o "$OUT" \
    test/sinc_resampler_test.cpp src/mixeroutput/sinc_resampler.cpp

"$OUT"
