#!/bin/sh
# One-command functional verification: configure, build, and run the Norn
# test suite in an isolated Debug build directory.
set -e

root=$(cd "$(dirname "$0")/.." && pwd)
build_dir=${NORN_BUILD_DIR:-"$root/build/verify"}

jobs=$(command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu)

cmake -S "$root" -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$build_dir" -j "$jobs"
ctest --test-dir "$build_dir" --output-on-failure
