#!/usr/bin/env bash
# Runs all host-side tests: C++ component (with ESPHome stubs) + Python tools.
# Requirements: g++ (C++17), python3. Optional: numpy + opencv-python for video tests.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p host/build

echo "== C++ component tests =="
g++ -std=gnu++17 -O1 -Wall -Wno-unused-parameter \
    -Ihost/stubs -I../components/ddp_display \
    host/test_component.cpp ../components/ddp_display/ddp_display.cpp \
    -o host/build/test_component -pthread
./host/build/test_component

echo
echo "== Python tools tests =="
python3 test_tools.py
