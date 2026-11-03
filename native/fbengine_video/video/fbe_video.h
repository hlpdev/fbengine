#ifndef FBE_VIDEO_H
#define FBE_VIDEO_H

// Videos, and what every area of fbengine_video's C API shares: how its functions are exported,
// and its errors. A video is decoded on a thread of its own, a few frames ahead of where it's
// played, into pixels in memory: 4 bytes a pixel, red, green, blue, and alpha, in rows from the
// top. It plays by a clock the game moves on, by the time a frame took or by its sound's position.
// Each area's header (fbe_<area>.h) has its own binding file in packages/fbengine_video/c/, which
// tools/bindgen.sh makes, so an area's header uses only fixed-size types and its own structs,
// never another area's. A program loads this DLL beside fbengine_native, so no function here is
// named as one of another package's is. Everything but fbe_video_version and fbe_video_error is the
// main thread's, which the FB API's @MainThread keeps it to.

#include <stdbool.h>
#include <stdint.h>

#ifndef _WIN32
#define FBE_VIDEO_API __attribute__((visibility("default")))
#else
#ifdef FBE_VIDEO_BUILDING
#define FBE_VIDEO_API __declspec(dllexport)
#else
#define FBE_VIDEO_API __declspec(dllimport)
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// The version of fbengine_video this DLL is, like "0.1.0", which is fbengine's.
FBE_VIDEO_API const char* fbe_video_version(void);

// What the last call that failed on this thread says went wrong.
FBE_VIDEO_API const char* fbe_video_error(void);

// A video of the `size` bytes at `data`, which it keeps what it needs of: an MPEG-1 program
// stream, as a .mpg is, of MPEG-1 video and, if it has sound, MPEG-1 audio layer II, its sound
// decoded now. Paused at its start, its first frame decoded; 0 if the bytes aren't a video of a
// format it decodes, or its sound is longer than a WAV holds.
FBE_VIDEO_API uint64_t fbe_video_open(const uint8_t* data, int64_t size);

// Frees `video`, stopping its thread; false if it isn't a video.
FBE_VIDEO_API bool fbe_video_free(uint64_t video);

// How many videos there are, opened and not freed.
FBE_VIDEO_API int32_t fbe_video_count(void);

// `video`'s width and height in pixels, frames a second, and length in seconds; 0 if it isn't a
// video.
FBE_VIDEO_API int32_t fbe_video_width(uint64_t video);
FBE_VIDEO_API int32_t fbe_video_height(uint64_t video);
FBE_VIDEO_API double fbe_video_frame_rate(uint64_t video);
FBE_VIDEO_API double fbe_video_duration(uint64_t video);

// How wide `video`'s pixels are shown against how high, as its file says, 1 for square, 0 if it
// isn't a video.
FBE_VIDEO_API double fbe_video_pixel_aspect(uint64_t video);

// Whether `video` has sound, and its frames a second and channels; 0 for a video without, or what
// isn't a video.
FBE_VIDEO_API bool fbe_video_has_sound(uint64_t video);
FBE_VIDEO_API int32_t fbe_video_sound_rate(uint64_t video);
FBE_VIDEO_API int32_t fbe_video_sound_channels(uint64_t video);

// `video`'s whole sound, as a WAV file of 32-bit float samples at its rate and channels, in up to
// `capacity` bytes at `bytes`, if there's room for it, giving how many bytes it takes; -1 if it
// isn't a video or has no sound. Its first sample is at the video's time 0, its first picture's,
// however its stream was cut, and it's silent where its stream has a gap, as its packets' times
// say, though a time more than the sound's own length and the video's, or 5 seconds if that's more,
// or 10 minutes if that's less, out is taken to be wrong; it was decoded as the video was opened.
FBE_VIDEO_API int64_t fbe_video_sound(uint64_t video, uint8_t* bytes, int64_t capacity);

// Plays `video` from where its clock is, or pauses it there; false if it isn't a video. A video
// that's ended plays from its start again.
FBE_VIDEO_API bool fbe_video_play(uint64_t video);
FBE_VIDEO_API bool fbe_video_pause(uint64_t video);

// Whether `video` is playing: not paused, and not ended.
FBE_VIDEO_API bool fbe_video_playing(uint64_t video);

// Makes `video` start again from its start each time it ends, or not, as at first; false if it
// isn't a video.
FBE_VIDEO_API bool fbe_video_set_loop(uint64_t video, bool loop);
FBE_VIDEO_API bool fbe_video_looping(uint64_t video);

// Moves a playing `video`'s clock on by `seconds`, 0 to a billion, as a frame took, showing the
// frame due then, and waiting for its thread to decode it if it's behind; false if it isn't a
// video or the time's out of range, as more than 31 years at once is a mistake, as a time in
// nanoseconds would be. A video that isn't looping ends as its clock passes its length; a looping
// one's clock is as exact however long it plays.
FBE_VIDEO_API bool fbe_video_update(uint64_t video, double seconds);

// Puts `video`'s clock at `seconds`, from 0 to its length, showing the frame due then, decoded from
// the key frame before it, as a seek or a sound's position does; false if it isn't a video or the
// time's out of range.
FBE_VIDEO_API bool fbe_video_seek(uint64_t video, double seconds);

// Where `video`'s clock is, from 0 to its length, -1 if it isn't a video; whether it's ended,
// played to its end and not looping.
FBE_VIDEO_API double fbe_video_time(uint64_t video);
FBE_VIDEO_API bool fbe_video_ended(uint64_t video);

// The number of the frame `video` shows, from 0, -1 if it isn't a video, and how many frames it's
// shown since it was opened, counting each once as it's shown, which says whether a new one came.
FBE_VIDEO_API int64_t fbe_video_frame_number(uint64_t video);
FBE_VIDEO_API int64_t fbe_video_frames_shown(uint64_t video);

// Copies the frame `video` shows into the `size` bytes at `pixels`, its width times its height
// times 4; false if it isn't a video or the size isn't that.
FBE_VIDEO_API bool fbe_video_frame(uint64_t video, uint8_t* pixels, int64_t size);

#ifdef __cplusplus
}
#endif

#endif
