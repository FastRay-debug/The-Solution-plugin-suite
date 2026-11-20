#!/bin/bash
set -e
cd "$(dirname "$0")"

LOCAL_DIR="$(pwd)/The Solution"
mkdir -p "$LOCAL_DIR/VST3" "$LOCAL_DIR/CLAP" "$LOCAL_DIR/AU" "$LOCAL_DIR/Standalone"
mkdir -p "$HOME/Library/Audio/Plug-Ins/VST3/The Solution"
mkdir -p "$HOME/Library/Audio/Plug-Ins/CLAP/The Solution"
mkdir -p "$HOME/Library/Audio/Plug-Ins/Components"
SKINS_DIR="$HOME/Library/Application Support/FastRayMedia/The Solution/Skins"
mkdir -p "$LOCAL_DIR/Skins" "$SKINS_DIR"
cp UI/skins/*.jpg "$LOCAL_DIR/Skins/"
cp UI/skins/*.jpg "$SKINS_DIR/"

echo "Gathering and installing all VST3, CLAP, AU, and Standalone builds..."
find build -name "*.vst3" -type d -exec cp -R {} "$LOCAL_DIR/VST3/" \; -exec cp -R {} "$HOME/Library/Audio/Plug-Ins/VST3/The Solution/" \;
find build -name "*.clap" -exec cp -R {} "$LOCAL_DIR/CLAP/" \; -exec cp -R {} "$HOME/Library/Audio/Plug-Ins/CLAP/The Solution/" \;
find build -name "*.component" -type d -exec cp -R {} "$LOCAL_DIR/AU/" \; -exec cp -R {} "$HOME/Library/Audio/Plug-Ins/Components/" \;
find build -path "*/Standalone/*.app" -type d -exec cp -R {} "$LOCAL_DIR/Standalone/" \;

echo "[SUCCESS] All plugins gathered in '$LOCAL_DIR' and installed to ~/Library/Audio/Plug-Ins (skins in $SKINS_DIR)!"
open "$LOCAL_DIR"
