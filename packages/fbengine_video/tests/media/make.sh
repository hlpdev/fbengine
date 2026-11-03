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
#   opengop.mpg: count.mpg's pictures with countb.mpg's B-frames, so its groups after the first
#               are open, and 2 s of pink noise at 64 kb/s.
#   wide.mpg:   count.mpg's pictures, its pixels shown for a shape of 16 by 9.
#   longgroup.mpg: 320 by 240, 30 frames a second, 4 s of gray noise in one group of 120
#               pictures, so a seek near its end decodes them all.
# Made from them by hand, as a review found them, and checked in as they are:
#   cutb.mpg:   countb.mpg's video cut after its last B-frame but one, so its last picture
#               decoded is a B-frame, remuxed without times.
#   gap.mpg:    colors.mpg with its sound's packets from 0.8 s to 1.3 s made padding.
#   longsound.mpg: colors.mpg with its last sound packet's time the most 33 bits hold, 26 hours.
#   opencut.mpg: opengop.mpg cut at its third group's sequence header, its times as they were:
#               its video from there in packets of 2000 bytes, the first without a time and each
#               after with the time of the first picture to start in it, and its sound's packets
#               from 1 s on, as a stream joined after its start is.
#   longpad.mpg: colors.mpg, then 20,000 bare picture start codes, 3 bytes each, which make it
#               802 s long, then a copy of its last sound packet at 6 s, at 30 s, and at 650 s.
#   late10.mpg: pictures, with a tone starting 10 s after the first of them.
#   cut_neg.mpg: opencut.mpg with its video's times 1.7 s earlier, so its first picture's time,
#               less its number's, is before 0.
#   cut_pos.mpg: the same, all its times 0.7 s later, so it isn't.
#   broken/:    videos that read or wrote outside the heap in pl_mpeg before its patches, one
#               of them its issue #68's; and zero1.mpg and zerotail.mpg, colors.mpg with its
#               first sound packet's data zeros, and a middle one's zeros from its last frame's
#               header on, which crashed fbengine_video's sound decoding a packet at a time.
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
ffmpeg -v error -y -f lavfi -i "$count" -f lavfi -i "anoisesrc=d=2:c=pink:r=44100:a=0.3:seed=7" \
    -ac 2 -c:v mpeg1video -q:v 2 -g 12 -bf 2 -sc_threshold 1000000000 -c:a mp2 -b:a 64k -f mpeg opengop.mpg
ffmpeg -v error -y -f lavfi -i "color=c=gray:s=320x240:r=30:d=4,noise=alls=20:allf=t" \
    -an -c:v mpeg1video -q:v 31 -g 120 -sc_threshold 1000000000 -f mpeg longgroup.mpg
ffmpeg -v error -y -f lavfi -i "$count" \
    -an -c:v mpeg1video -q:v 2 -g 12 -aspect 16:9 -sc_threshold 1000000000 -f mpeg wide.mpg
