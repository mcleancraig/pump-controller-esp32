#!/usr/bin/env bash
set -e

SKETCH="pump-controller-esp32.ino"
FQBN="esp32:esp32:waveshare_esp32_c6_zero"

arduino-cli compile \
  --fqbn "$FQBN" \
  --output-dir build/ \
  "$SKETCH"

echo "Build complete. Binary: build/${SKETCH%.ino}.ino.bin"
