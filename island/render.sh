#!/usr/bin/env bash
# Renders the island flythrough and encodes it to H.264.
# Requires g++ with OpenMP and ffmpeg (e.g. `pip install imageio-ffmpeg`).
set -euo pipefail
cd "$(dirname "$0")"
W=${W:-1280} H=${H:-720} SPP=${SPP:-2} FRAMES=${FRAMES:-540}
FRAMEDIR=${FRAMEDIR:-frames}
FFMPEG=${FFMPEG:-$(command -v ffmpeg || python3 -c "import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())")}
g++ -O3 -march=native -fopenmp -o island island.cpp
./island "$FRAMEDIR" "$W" "$H" "$SPP" 0 $((FRAMES - 1)) "$FRAMES"   # resumable: skips finished frames
"$FFMPEG" -y -framerate 30 -i "$FRAMEDIR/f%04d.ppm" -c:v libx264 -preset slow -crf 17 \
  -pix_fmt yuv420p -movflags +faststart island_flythrough.mp4
