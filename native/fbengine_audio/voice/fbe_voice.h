#ifndef FBE_VOICE_H
#define FBE_VOICE_H

// Voice: what voice chat needs between a microphone and the network. Opus compresses speech to
// a few kilobytes a second and back, hiding a packet that's lost; RNNoise takes out the noise
// around a voice, a fan, a keyboard, hiss, and says how likely each stretch is a voice. Encoders,
// decoders, and denoisers are handles, apart from the mix, which needn't be started.

#include "../audio/fbe_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

// The frames a denoiser takes at a time: 10 ms at 48000 Hz, mono.
#define FBE_VOICE_DENOISE_FRAMES 480

// An Opus encoder for speech at `sample_rate` (8000, 12000, 16000, 24000, or 48000 Hz) and
// `channels` (1 or 2), compressing to about `bitrate` bits a second (6000 to 510000); 0 if a
// value's out of range.
FBE_AUDIO_API uint64_t fbe_voice_encoder_add(int32_t sample_rate, int32_t channels,
                                             int32_t bitrate);
// Sets the share of packets, 0 to 100 percent, the encoder expects to be lost: above 0, each
// packet carries a rougher copy of the one before, which a decoder takes in its place when it's
// lost (fbe_voice_decode's `fec`). False if it's out of range or the encoder's gone.
FBE_AUDIO_API bool fbe_voice_set_loss(uint64_t encoder, int32_t percent);
// Compresses `frames` frames at `samples`, its channels' samples each, from -1 to 1, into a packet
// of up to `room` bytes at `packet`, at less quality if it must to fit (4000 bytes never needs
// to): the packet's bytes, or -1 if the encoder's gone, or `frames` isn't 2.5, 5, 10, 20, 40, or
// 60 ms of them.
FBE_AUDIO_API int32_t fbe_voice_encode(uint64_t encoder, const float* samples, int32_t frames,
                                       uint8_t* packet, int32_t room);
// An Opus decoder for what an encoder of `sample_rate` and `channels` compressed, as
// fbe_voice_encoder_add takes; 0 if a value's out of range.
FBE_AUDIO_API uint64_t fbe_voice_decoder_add(int32_t sample_rate, int32_t channels);
// Decodes the `size` bytes of `packet` into up to `frames` frames at `samples`: how many, or -1 if
// the decoder's gone or the packet's corrupt, or is more than `frames`. With no packet (null, or
// 0 bytes), it makes up `frames` frames in place of one that's lost, from what came before; with
// `fec`, it decodes the packet before `packet`, lost, from the copy of it `packet` carries, as
// many frames as it had, `frames`.
FBE_AUDIO_API int32_t fbe_voice_decode(uint64_t decoder, const uint8_t* packet, int32_t size,
                                       bool fec, float* samples, int32_t frames);
// A denoiser, RNNoise's, which keeps what it's heard to hear the next.
FBE_AUDIO_API uint64_t fbe_voice_denoiser_add(void);
// Takes the noise out of the FBE_VOICE_DENOISE_FRAMES frames at `samples`, mono at 48000 Hz,
// from -1 to 1, in place: how likely they're a voice, 0 to 1, or -1 if the denoiser's gone.
FBE_AUDIO_API float fbe_voice_denoise(uint64_t denoiser, float* samples);
// Lets an encoder, decoder, or denoiser go; one that's gone already is nothing.
FBE_AUDIO_API void fbe_voice_free(uint64_t handle);
// How many encoders, decoders, and denoisers there are, those not freed.
FBE_AUDIO_API int32_t fbe_voice_count(void);

#ifdef __cplusplus
}
#endif

#endif
