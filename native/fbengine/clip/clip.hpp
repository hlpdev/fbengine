#pragma once

// Clips, as the clip area keeps them, and sampling one: each channel's keys at a time, as glTF
// interpolates them. The layout is assets' decoded clip_t's, with the names of the nodes its
// channels move, so a clip made in code, or a file's, plays on any hierarchy with those names.

#include "scene/math.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fbe::anim {

/// What of a node a channel moves.
enum class property_t : std::uint8_t { TRANSLATION, ROTATION, SCALE, WEIGHTS };
/// How a channel goes between its keys: holding the earlier, in a line (the short way round for a
/// rotation), or on glTF's cubic spline through tangents.
enum class interpolation_t : std::uint8_t { STEP, LINEAR, CUBIC };

/// What a clip moves of one node: its keys' times, in seconds, rising, and values, each `width`
/// floats (3, 4 for a rotation's x, y, z, w, or a weight for each morph target), and for CUBIC the
/// in and out tangents around each, as in, value, out.
struct channel_t {
    /// Its node, by index into its clip's names.
    std::uint32_t node = 0;
    property_t property = property_t::TRANSLATION;
    interpolation_t interpolation = interpolation_t::LINEAR;
    std::uint32_t width = 3;
    std::vector<float> times;
    std::vector<float> values;
};

/// A time in a clip a player reports reaching, by its name.
struct event_t {
    float time = 0.0F;
    std::string name;
};

struct clip_t {
    std::string name;
    /// The names of the nodes its channels move, each once.
    std::vector<std::string> nodes;
    std::vector<channel_t> channels;
    /// Its events, in the order of their times, those at one time in the order they were added.
    std::vector<event_t> events;
    /// Its last key's or event's time.
    float length = 0.0F;
    /// The frames a second it was made at, or 0 for none given.
    float frame_rate = 0.0F;
};

/// A clip as the clip area keeps it: shared by its handle and every player playing it, which its
/// references count, and how many times it changed, so a player bound to it knows to bind again.
struct kept_clip_t {
    clip_t clip;
    std::uint32_t references = 0;
    std::uint64_t version = 0;
};

/// The live clip a handle is for, or none, with fbe_error saying so.
std::uint32_t clip_slot(std::uint64_t handle);
/// The clip in a live slot, and its handle.
kept_clip_t& kept_clip(std::uint32_t slot);
std::uint64_t clip_handle(std::uint32_t slot);
/// Holds a clip once more, or lets it go once, freeing it after its last.
void retain_clip(std::uint32_t slot);
void release_clip(std::uint32_t slot);

/// The key whose time is the last at or before `time`, 0 before the first, starting from `hint`,
/// the one found last, so playing on finds the next in a step or two; a jump searches.
std::size_t key_at(std::span<const float> times, float time, std::size_t hint);

/// A channel's value at `time`, its `width` floats written to `out`, `cursor` its key found last,
/// which it updates. Before its first key it holds that key's value, and after its last that one's.
void sample(const channel_t& channel, float time, std::size_t& cursor, std::span<float> out);

/// The part of `clip` from `start` to `end` seconds, 0 or more and the one after the other, moved
/// to start at 0: each channel's keys between them, with a key at each end, which sample as the
/// clip did there, and its events between them or at either end.
clip_t slice(const clip_t& clip, float start, float end);

} // namespace fbe::anim
