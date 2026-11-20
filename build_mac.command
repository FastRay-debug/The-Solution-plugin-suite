#!/bin/bash
set -e
cd "$(dirname "$0")"
echo "===================================================="
echo "  The Solution Suite - One-Click macOS Builder"
echo "===================================================="

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel

echo ""
echo "[SUCCESS] Build complete! Opening Standalone apps..."
open "build/TheStage_artefacts/Release/Standalone"
