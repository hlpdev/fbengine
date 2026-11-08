#!/bin/sh
# Makes the decoders' fuzz corpus, the seeds tests/audio/fuzz_tests.cpp changes and cuts short:
# a tenth of a second of a rising sweep with a little noise, in each format and encoding miniaudio
# reads (WAV's PCM, float, a-law, mu-law, and ADPCM, and AIFF, W64, and RF64; FLAC, alone and in
# Ogg; Ogg Vorbis), and Ogg Opus, which it doesn't. Run it from this folder with ffmpeg on PATH;
# the repository's own, made by this alone.
set -e
sweep='aevalsrc=0.4*sin(2*PI*(200+3000*t)*t)+0.05*(random(0)-0.5):s=48000:d=0.1'
make() {
    out="$1"
    shift
    ffmpeg -hide_banner -loglevel error -f lavfi -i "$sweep" -bitexact -map_metadata -1 "$@" -y "$out"
}
make wav_u8_mono_8000.wav -ar 8000 -ac 1 -c:a pcm_u8
make wav_s16_stereo_44100.wav -ar 44100 -ac 2 -c:a pcm_s16le
make wav_s24_mono_48000.wav -ar 48000 -ac 1 -c:a pcm_s24le
make wav_s32_stereo_22050.wav -ar 22050 -ac 2 -c:a pcm_s32le
make wav_f32_stereo_48000.wav -ar 48000 -ac 2 -c:a pcm_f32le
make wav_f64_mono_32000.wav -ar 32000 -ac 1 -c:a pcm_f64le
make wav_alaw_mono_8000.wav -ar 8000 -ac 1 -c:a pcm_alaw
make wav_mulaw_mono_8000.wav -ar 8000 -ac 1 -c:a pcm_mulaw
make wav_ima_adpcm_stereo_22050.wav -ar 22050 -ac 2 -c:a adpcm_ima_wav
make wav_ms_adpcm_mono_22050.wav -ar 22050 -ac 1 -c:a adpcm_ms
make wav_s16_51_48000.wav -af 'pan=5.1|c0=c0|c1=c0|c2=c0|c3=c0|c4=c0|c5=c0' -c:a pcm_s16le
make flac_s16_stereo_44100_level0.flac -ar 44100 -ac 2 -c:a flac -compression_level 0
make flac_s24_mono_48000_level12.flac -ar 48000 -ac 1 -c:a flac -sample_fmt s32 \
    -bits_per_raw_sample 24 -compression_level 12
make flac_s16_stereo_96000_small_frames.flac -ar 96000 -ac 2 -c:a flac -frame_size 256
make ogg_vorbis_mono_8000_q0.ogg -ar 8000 -ac 1 -c:a libvorbis -q:a 0
# Pages of 10 ms, where 1 s's are ffmpeg's own, so a seek has pages to find by their CRCs.
make ogg_vorbis_stereo_44100_small_pages.ogg -ar 44100 -ac 2 -c:a libvorbis -q:a 4 -page_duration 10000
make ogg_vorbis_stereo_44100_q6.ogg -ar 44100 -ac 2 -c:a libvorbis -q:a 6
make ogg_vorbis_51_48000.ogg -af 'pan=5.1|c0=c0|c1=c0|c2=c0|c3=c0|c4=c0|c5=c0' -c:a libvorbis \
    -q:a 2
make ogg_opus_stereo_48000.ogg -ac 2 -c:a libopus
# The other containers miniaudio's WAV and FLAC decoders read: AIFF, Sony Wave64, RF64 (a WAV
# with 64-bit sizes), and FLAC in Ogg pages.
make aiff_s16_stereo_44100.aiff -ar 44100 -ac 2 -c:a pcm_s16be
make w64_s16_stereo_44100.w64 -ar 44100 -ac 2 -c:a pcm_s16le
make rf64_s16_stereo_44100.wav -ar 44100 -ac 2 -c:a pcm_s16le -rf64 always
make ogg_flac_s16_stereo_44100.ogg -ar 44100 -ac 2 -c:a flac
# A WAV with a LIST chunk of tags before its data, which -map_metadata -1 would leave out.
ffmpeg -hide_banner -loglevel error -f lavfi -i "$sweep" -bitexact -ar 44100 -ac 1 -c:a pcm_s16le \
    -metadata title=corpus -metadata artist=fbengine -y wav_s16_mono_list_chunk.wav
