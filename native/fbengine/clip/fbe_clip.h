#ifndef FBE_CLIP_H
#define FBE_CLIP_H

// Clips: what moves the nodes of a hierarchy over time, each node by its name, so one plays on any
// hierarchy with those names. A clip is made in code, a channel at a time, or from a model's file,
// and every player of it shares it. It lives while its handle is held, as fbe_clip_create and
// fbe_clip_retain hold it and fbe_clip_release lets go, or a player plays it.
//
// A channel moves one property of one node through keys: their times, in seconds, rising, and
// their values, as glTF has them. A rotation is x, y, z, w; morph weights are one a target. A
// CUBIC channel's keys are each in tangent, value, and out tangent, as glTF's are.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// What a channel moves: a node's position, rotation, or scale, or its morph targets' weights.
#define FBE_CLIP_TRANSLATION 0
#define FBE_CLIP_ROTATION    1
#define FBE_CLIP_SCALE       2
#define FBE_CLIP_WEIGHTS     3

// How a channel goes between its keys: holding the earlier, in a line (a rotation the short way
// round), or on glTF's cubic spline through each key's tangents.
#define FBE_CLIP_STEP   0
#define FBE_CLIP_LINEAR 1
#define FBE_CLIP_CUBIC  2

// A new clip with no channels, named, at `frame_rate` frames a second, 0 for none; 0 when that's
// below 0 or not finite.
FBE_API uint64_t fbe_clip_create(const char* name, float frame_rate);
FBE_API bool fbe_clip_retain(uint64_t clip);
FBE_API bool fbe_clip_release(uint64_t clip);
FBE_API bool fbe_clip_alive(uint64_t clip);

// Adds a channel moving `property` of the node named `node`, with `keys` keys: `times`, `keys` of
// them, rising, and `values`, `width` floats each, three times that for CUBIC. A position or scale
// is 3 wide and a rotation 4; weights are as wide as they're given, 1 or more. False for a clip
// freed, a property or interpolation that isn't one, no keys, times that aren't finite or fall,
// values that aren't finite, a width that isn't the property's, or a node already moved in that
// property.
FBE_API bool fbe_clip_add_channel(uint64_t clip, const char* node, int32_t property,
                                  int32_t interpolation, int32_t width, const float* times,
                                  const float* values, int32_t keys);

// A new clip of clip `index` of a model load's glTF, its channels' nodes by their names, so it
// plays on the entities made from the model and on any others with those names; a channel of an
// unnamed node moves nothing. 0 for a load that isn't a model load done and loaded, an index out
// of range, or what's in the file: keys a clip made in code couldn't have, or one name's property
// moved twice.
FBE_API uint64_t fbe_clip_from_model(uint64_t load, int32_t index);
// Whether the last fbe_clip_from_model gave 0 for what's in the file, rather than how it was asked.
FBE_API bool fbe_clip_model_invalid(void);

// Its name, valid until the next clip's made, or "" for a freed clip.
FBE_API const char* fbe_clip_name(uint64_t clip);
// Adds an event named `name` at `time` seconds, which a player playing the clip reports reaching
// there; a clip's events can share a time, or a name. False for a freed clip, an empty name, or a
// time below 0 or not finite.
FBE_API bool fbe_clip_add_event(uint64_t clip, const char* name, float time);
// How many events it has, 0 for a freed clip; event `event`'s name, valid until the clip changes
// or is freed, and time, by the order of their times, "" and 0 for one out of range.
FBE_API int32_t fbe_clip_event_count(uint64_t clip);
FBE_API const char* fbe_clip_event_name(uint64_t clip, int32_t event);
FBE_API float fbe_clip_event_time(uint64_t clip, int32_t event);

// A new clip of the part of `clip` from `start` to `end` seconds, moved to start at 0, which
// samples as `clip` did there: each channel's keys between them, with a key at each end, a cubic
// spline cut along its slope, and the events between them or at either end. 0 for a freed clip, or
// a part that isn't finite, starts below 0, or ends before it starts.
FBE_API uint64_t fbe_clip_slice(uint64_t clip, float start, float end);

// Its length, its last key's or event's time, 0 with none; its frames a second; how many channels
// it has.
FBE_API float fbe_clip_length(uint64_t clip);
FBE_API float fbe_clip_frame_rate(uint64_t clip);
FBE_API int32_t fbe_clip_channel_count(uint64_t clip);

// Channel `channel`'s value at `time`, its width of floats written to `out`, which has room for
// `capacity`; gives the width, or -1 for a freed clip, or a channel or capacity out of range.
FBE_API int32_t fbe_clip_sample(uint64_t clip, int32_t channel, float time, float* out,
                                int32_t capacity);

#ifdef __cplusplus
}
#endif

#endif
