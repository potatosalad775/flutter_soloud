#!/bin/bash
# Build & run the standalone peaking equalizer correctness tests.
# Run from the flutter_soloud repo root.

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

OUT="${TMPDIR:-/tmp}/peaking_eq_test"

# The real FilterInstance and Fader are linked rather than stubbed, because the
# transition tests need a genuine parameter fade to drive the filter.
c++ -std=c++17 -O2 -Wall -DNOMINMAX \
    -I src/soloud/include \
    -o "$OUT" \
    test/peaking_eq_test.cpp src/filters/peaking_eq_filter.cpp \
    src/soloud/src/core/soloud_filter.cpp \
    src/soloud/src/core/soloud_fader.cpp

"$OUT"
