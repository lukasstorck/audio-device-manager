#!/usr/bin/env bash
set -e

/usr/bin/cmake -S . -B build -G Ninja
/usr/bin/cmake --build build
/usr/bin/ctest --test-dir build --output-on-failure
