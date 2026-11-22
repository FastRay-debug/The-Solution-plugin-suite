#!/usr/bin/env bash
# Fetches the offline face-tracking runtime + model for the "Webcam head tracking" install option.
# Usage: packaging/fetch_facetracking.sh <out dir>
set -euo pipefail
out="${1:-The Solution/FaceTracking}"; mkdir -p "$out"; tmp="$(mktemp -d)"
( cd "$tmp" && npm pack @mediapipe/tasks-vision@0.10.21 >/dev/null && tar xzf mediapipe-tasks-vision-0.10.21.tgz )
cp "$tmp/package/vision_bundle.mjs" "$tmp/package/wasm/vision_wasm_internal.js" "$tmp/package/wasm/vision_wasm_internal.wasm" \
   "$tmp/package/wasm/vision_wasm_nosimd_internal.js" "$tmp/package/wasm/vision_wasm_nosimd_internal.wasm" "$out/"
curl -fsSL -o "$out/face_landmarker.task" https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task
cp "$tmp/package/README.md" "$out/MEDIAPIPE_README.md" 2>/dev/null || true
echo "Apache-2.0 - MediaPipe Tasks Vision (Google). https://github.com/google-ai-edge/mediapipe" > "$out/LICENSE-MEDIAPIPE.txt"
rm -rf "$tmp"; ls -la "$out"
