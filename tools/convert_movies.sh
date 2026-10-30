#!/bin/sh
# Convert the game's movies (MPEG-4 Part 2, up to 1366x720, which the Vita cannot decode in hardware) to H.264 that
# its decoder plays: 960x544, H.264 Constrained Baseline (no B-frames, one reference frame; r62 - the Main-profile
# files never started: the player stopped at once), AAC-LC 48 kHz. Letterboxed/pillarboxed to keep the picture's shape.
#
# Usage:  tools/convert_movies.sh <folder with the .m4v files> <output folder>
#   The .m4v files are in the main expansion file (a zip): main.69.com.phosphorgames.DarkMeadow.obb ->
#   UDKGame/Movies/{Startup,Ending,end_meadow,end_psychWard}.m4v
# Copy the resulting .mp4 files to ux0:data/darkmeadow/movies/ on the Vita.
# Needs ffmpeg (with libx264).
set -e
in="${1:?folder with the .m4v files}"
out="${2:?output folder}"
mkdir -p "$out"
for f in "$in"/*.m4v; do
    name=$(basename "$f" .m4v)
    echo "== $name"
    ffmpeg -y -i "$f" \
        -vf "scale=960:544:force_original_aspect_ratio=decrease,pad=960:544:(ow-iw)/2:(oh-ih)/2:black,format=yuv420p" \
        -c:v libx264 -profile:v baseline -level 3.1 -bf 0 -refs 1 -preset medium -b:v 2500k -maxrate 3000k -bufsize 3000k \
        -r 30 -g 30 \
        -c:a aac -profile:a aac_low -b:a 128k -ar 48000 -ac 2 \
        -brand mp42 -movflags +faststart "$out/$name.mp4"
done
echo "Done. Copy $out/*.mp4 to ux0:data/darkmeadow/movies/"
