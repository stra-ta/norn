#!/bin/sh
# Full functional confidence: run the suite in Debug, under AddressSanitizer,
# and under UndefinedBehaviorSanitizer, each in its own isolated build
# directory. TSan is exercised separately by the tsan preset because the
# weakened-order probe tests belong to that profile.
set -e

for preset in default asan ubsan; do
    echo "=== $preset ==="
    cmake --preset "$preset"
    cmake --build --preset "$preset" -j "$(command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu)"
    ctest --preset "$preset" --output-on-failure
done
