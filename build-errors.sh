#!/usr/bin/env bash
# Script: capture_build_errors.sh
# Purpose: Run hyprscroller build and redirect all output to a timestamped file.
# Usage: ./capture_build_errors.sh [clean|debug|release]

set -euo pipefail

# Determine build target (default: release)
TARGET="${1:-release}"
case "$TARGET" in
clean | debug | release) ;;
*)
  echo "Usage: $0 [clean|debug|release]"
  exit 1
  ;;
esac

# Output file with timestamp
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTPUT_FILE="build_error_${TIMESTAMP}.txt"

echo "Starting build target: $TARGET"
echo "Output will be saved to: $OUTPUT_FILE"

# Run make and capture both stdout and stderr
if [[ "$TARGET" == "clean" ]]; then
  make clean >"$OUTPUT_FILE" 2>&1
else
  make "$TARGET" >"$OUTPUT_FILE" 2>&1
fi

# Check exit status
if [ $? -eq 0 ]; then
  echo "Build succeeded (no errors). Output saved to $OUTPUT_FILE"
else
  echo "Build FAILED. Check $OUTPUT_FILE for details."
fi
