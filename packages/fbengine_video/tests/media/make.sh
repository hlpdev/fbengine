#!/bin/sh
# Makes the test videos again with ffmpeg; the tests read the ones checked in, never this.
#   colors.mpg: 64 by 48, 25 frames a second, 2 s: half a second each of red, green, blue, and
#               white, with a 440 Hz tone in both channels at 44100 Hz.
#   silent.mpg: the same pictures, with no sound.
#   late.mpg:   the same, its tone starting half a second after its pictures.
#   count.mpg:  64 by 48, 25 frames a second, 2 s, each frame's luma 16 and 4 times its number,
#               so its gray is about 4.66 times it, a key frame each 12 and the rest P-frames
#               (ffmpeg would make each a key frame, each so unlike the one before).
#   countb.mpg: the same, with two B-frames before each P-frame.
#   cgop.mpg:   the same, its groups of pictures closed, so its stream ends with B-frames.
#   ntsc.mpg:   64 by 48, 30000/1001 frames a second, 4 s, counting as count.mpg does from 0 to
#               49 and again, three B-frames before each P-frame.
#   busy.mpg:   160 by 120, 60 frames a second, 2 s, of ffmpeg's moving test pattern, with two
#               B-frames before each P-frame, and a tone.
# Made from them by hand, as a review found them, and checked in as they are:
#   cutb.mpg:   countb.mpg's video cut after its last B-frame but one, so its last picture
#               decoded is a B-frame, remuxed without times.
#   gap.mpg:    colors.mpg with its sound's packets from 0.8 s to 1.3 s made padding.
#   longsound.mpg: colors.mpg with its last sound packet's time the most 33 bits hold, 26 hours.
#   broken/:    videos that read or wrote outside the heap in pl_mpeg before its patches, one
#               of them its issue #68's.
set -eu
cd "$(dirname "$0")"
colors="color=c=red:s=64x48:r=25:d=0.5[r];color=c=lime:s=64x48:r=25:d=0.5[g];color=c=blue:s=64x48:r=25:d=0.5[b];color=c=white:s=64x48:r=25:d=0.5[w];[r][g][b][w]concat=n=4:v=1:a=0"
tone="sine=frequency=440:sample_rate=44100:duration=2"
count="color=c=black:s=64x48:r=25:d=2,format=yuv420p,geq=lum='16+N*4':cb=128:cr=128"
ffmpeg -v error -y -f lavfi -i "$colors" -f lavfi -i "$tone" \
    -ac 2 -c:v mpeg1video -q:v 2 -g 12 -c:a mp2 -b:a 128k -f mpeg colors.mpg
ffmpeg -v error -y -f lavfi -i "$colors" -an -c:v mpeg1video -q:v 2 -g 12 -f mpeg silent.mpg
ffmpeg -v error -y -f lavfi -i "$colors" -itsoffset 0.5 -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=1.5" \
    -ac 2 -c:v mpeg1video -q:v 2 -g 12 -c:a mp2 -b:a 128k -f mpeg late.mpg
ffmpeg -v error -y -f lavfi -i "$count" \
    -an -c:v mpeg1video -q:v 2 -g 12 -sc_threshold 1000000000 -f mpeg count.mpg
ffmpeg -v error -y -f lavfi -i "$count" \
    -an -c:v mpeg1video -q:v 2 -g 12 -bf 2 -sc_threshold 1000000000 -f mpeg countb.mpg
ffmpeg -v error -y -f lavfi -i "$count" \
    -an -c:v mpeg1video -q:v 2 -g 12 -bf 2 -flags +cgop -sc_threshold 1000000000 -f mpeg cgop.mpg
ffmpeg -v error -y -f lavfi -i "color=c=black:s=64x48:r=30000/1001:d=4,format=yuv420p,geq=lum='16+mod(N\,50)*4':cb=128:cr=128" \
    -an -c:v mpeg1video -q:v 2 -g 15 -bf 3 -sc_threshold 1000000000 -f mpeg ntsc.mpg
ffmpeg -v error -y -f lavfi -i "testsrc2=s=160x120:r=60:d=2" -f lavfi -i "$tone" \
    -ac 2 -c:v mpeg1video -q:v 12 -g 18 -bf 2 -c:a mp2 -b:a 64k -f mpeg busy.mpg
