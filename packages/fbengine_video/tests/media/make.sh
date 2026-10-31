#!/bin/sh
# Makes the test videos again with ffmpeg; the tests read the ones checked in, never this.
#   colors.mpg: 64 by 48, 25 frames a second, 2 s: half a second each of red, green, blue, and
#               white, with a 440 Hz tone in both channels at 44100 Hz.
#   silent.mpg: the same pictures, with no sound.
#   count.mpg:  64 by 48, 25 frames a second, 2 s, each frame's luma 16 and 4 times its number,
#               so its gray is about 4.66 times it, a key frame each 12 and the rest P-frames
#               (ffmpeg would make each a key frame, each so unlike the one before).
#   countb.mpg: the same, with two B-frames before each P-frame.
set -eu
cd "$(dirname "$0")"
colors="color=c=red:s=64x48:r=25:d=0.5[r];color=c=lime:s=64x48:r=25:d=0.5[g];color=c=blue:s=64x48:r=25:d=0.5[b];color=c=white:s=64x48:r=25:d=0.5[w];[r][g][b][w]concat=n=4:v=1:a=0"
ffmpeg -v error -y -f lavfi -i "$colors" -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2" \
    -ac 2 -c:v mpeg1video -q:v 2 -g 12 -c:a mp2 -b:a 128k -f mpeg colors.mpg
ffmpeg -v error -y -f lavfi -i "$colors" -an -c:v mpeg1video -q:v 2 -g 12 -f mpeg silent.mpg
ffmpeg -v error -y -f lavfi -i "color=c=black:s=64x48:r=25:d=2,format=yuv420p,geq=lum='16+N*4':cb=128:cr=128" \
    -an -c:v mpeg1video -q:v 2 -g 12 -sc_threshold 1000000000 -f mpeg count.mpg
ffmpeg -v error -y -f lavfi -i "color=c=black:s=64x48:r=25:d=2,format=yuv420p,geq=lum='16+N*4':cb=128:cr=128"     -an -c:v mpeg1video -q:v 2 -g 12 -bf 2 -sc_threshold 1000000000 -f mpeg countb.mpg
