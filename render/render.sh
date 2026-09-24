#!/usr/bin/env bash
# Renders the Newton's cradle animation and encodes it to H.264.
# Requires g++ with OpenMP and ffmpeg (e.g. `pip install imageio-ffmpeg`).
set -euo pipefail
cd "$(dirname "$0")"
W=${W:-1280} H=${H:-720} SPP=${SPP:-128} FRAMES=${FRAMES:-240}
FRAMEDIR=${FRAMEDIR:-frames}
FFMPEG=${FFMPEG:-$(command -v ffmpeg || python3 -c "import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())")}
g++ -O3 -march=native -fopenmp -o cradle cradle.cpp
./cradle "$FRAMEDIR" "$W" "$H" "$SPP" 0 $((FRAMES - 1)) "$FRAMES"   # resumable: skips finished frames
"$FFMPEG" -y -framerate 30 -i "$FRAMEDIR/f%04d.ppm" -c:v libx264 -preset slow -crf 17 \
  -pix_fmt yuv420p -movflags +faststart newtons_cradle.mp4
