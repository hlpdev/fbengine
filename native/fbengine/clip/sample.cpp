// Sampling a clip's channels: finding the keys around a time, and going between them as glTF says,
// a rotation's the short way round and back to length 1.

#include "clip/clip.hpp"
#include "scene/math.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace fbe::anim {

namespace {

/// How many keys on from the last one playing on looks through before it searches instead.
constexpr std::size_t nearby = 4;

/// The value of key `key`, its middle third for CUBIC.
std::span<const float> value_of(const channel_t& channel, std::size_t key) {
    const std::size_t width = channel.width;
    const std::size_t stride = channel.interpolation == interpolation_t::CUBIC ? width * 3 : width;
    const std::size_t at = channel.interpolation == interpolation_t::CUBIC ? width : 0;
    return std::span<const float>(channel.values).subspan((key * stride) + at, width);
}

/// A rotation of four floats, x, y, z, w.
scene::quat_t quat_of(std::span<const float> v) {
    return { v[0], v[1], v[2], v[3] };
}

void write(std::span<float> out, scene::quat_t q) {
    out[0] = q.x;
    out[1] = q.y;
    out[2] = q.z;
    out[3] = q.w;
}

/// glTF's cubic Hermite spline from key `key` to the next, `s` of the way, its tangents scaled by
/// the time between them.
void cubic(const channel_t& channel, std::size_t key, float s, float gap, std::span<float> out) {
    const std::size_t width = channel.width;
    const std::span<const float> values(channel.values);
    const std::span<const float> from = values.subspan(key * width * 3, width * 3);
    const std::span<const float> to = values.subspan((key + 1) * width * 3, width * 3);
    const float s2 = s * s;
    const float s3 = s2 * s;
    const float from_value = (2.0F * s3) - (3.0F * s2) + 1.0F;
    const float from_out = (s3 - (2.0F * s2) + s) * gap;
    const float to_value = (-2.0F * s3) + (3.0F * s2);
    const float to_in = (s3 - s2) * gap;
    for (std::size_t i = 0; i < width; ++i) {
        out[i] = (from_value * from[width + i]) + (from_out * from[(2 * width) + i]) +
                 (to_value * to[width + i]) + (to_in * to[i]);
    }
}

} // namespace

std::size_t key_at(std::span<const float> times, float time, std::size_t hint) {
    if (times.empty() || time <= times.front()) {
        return 0;
    }
    if (hint < times.size() && times[hint] <= time) {
        for (std::size_t step = 0; step < nearby; ++step) {
            if (hint + 1 >= times.size() || times[hint + 1] > time) {
                return hint;
            }
            ++hint;
        }
    }
    // The first key past `time`, less one.
    const auto past = std::ranges::upper_bound(times, time);
    return static_cast<std::size_t>(past - times.begin()) - 1;
}

void sample(const channel_t& channel, float time, std::size_t& cursor, std::span<float> out) {
    const std::span<const float> times(channel.times);
    const std::size_t width = channel.width;
    if (times.empty()) {
        return;
    }
    const std::size_t key = key_at(times, time, cursor);
    cursor = key;
    const bool rotation = channel.property == property_t::ROTATION;
    // Before the first key, at the last, or past it: that key's value.
    if (time <= times[key] || key + 1 >= times.size()) {
        const std::span<const float> held = value_of(channel, key);
        std::copy_n(held.begin(), width, out.begin());
        if (rotation) {
            write(out, scene::normalize(quat_of(out)));
        }
        return;
    }
    if (channel.interpolation == interpolation_t::STEP) {
        std::copy_n(value_of(channel, key).begin(), width, out.begin());
        if (rotation) {
            write(out, scene::normalize(quat_of(out)));
        }
        return;
    }
    const float gap = times[key + 1] - times[key];
    const float s = std::clamp((time - times[key]) / gap, 0.0F, 1.0F);
    if (channel.interpolation == interpolation_t::CUBIC) {
        cubic(channel, key, s, gap, out);
        if (rotation) {
            write(out, scene::normalize(quat_of(out)));
        }
        return;
    }
    const std::span<const float> from = value_of(channel, key);
    const std::span<const float> to = value_of(channel, key + 1);
    if (rotation) {
        write(out, scene::normalize(scene::slerp(quat_of(from), quat_of(to), s)));
        return;
    }
    for (std::size_t i = 0; i < width; ++i) {
        out[i] = from[i] + ((to[i] - from[i]) * s);
    }
}

} // namespace fbe::anim
