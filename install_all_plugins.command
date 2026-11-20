#!/bin/bash
set -e
cd "$(dirname "$0")"

LOCAL_DIR="$(pwd)/The Solution"
mkdir -p "$LOCAL_DIR/VST3" "$LOCAL_DIR/CLAP" "$LOCAL_DIR/AU" "$LOCAL_DIR/Standalone"
mkdir -p "$HOME/Library/Audio/Plug-Ins/VST3/The Solution"
mkdir -p "$HOME/Library/Audio/Plug-Ins/CLAP/The Solution"
mkdir -p "$HOME/Library/Audio/Plug-Ins/Components"

echo "Gathering and installing all VST3, CLAP, AU, and Standalone builds..."
find build -name "*.vst3" -type d -exec cp -R {} "$LOCAL_DIR/VST3/" \; -exec cp -R {} "$HOME/Library/Audio/Plug-Ins/VST3/The Solution/" \;
find build -name "*.clap" -exec cp -R {} "$LOCAL_DIR/CLAP/" \; -exec cp -R {} "$HOME/Library/Audio/Plug-Ins/CLAP/The Solution/" \;
find build -name "*.component" -type d -exec cp -R {} "$LOCAL_DIR/AU/" \; -exec cp -R {} "$HOME/Library/Audio/Plug-Ins/Components/" \;
find build -path "*/Standalone/*.app" -type d -exec cp -R {} "$LOCAL_DIR/Standalone/" \;

echo "[SUCCESS] All plugins gathered in '$LOCAL_DIR' and installed to ~/Library/Audio/Plug-Ins!"
open "$LOCAL_DIR"
