// Slicing a clip: the part between two times, moved to start at 0, which samples as the clip did
// there. Each channel keeps its keys between, with a key at each end: a cubic spline's cut where
// it's going, so the part is the same curve.

#include "clip/clip.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

namespace fbe::anim {

namespace {

/// A CUBIC channel's value and its slope, in units a second, at `time`, as it arrives there, at a
/// part's end, or leaves, at its start, its rotation not made length 1, so the part's keys are the
/// curve's own. Outside its keys, and leaving its last or arriving at its first, it holds, with a
/// slope of 0.
void curve_at(const channel_t& channel, float time, bool arriving, std::span<float> value,
              std::span<float> slope) {
    const std::span<const float> times(channel.times);
    const std::span<const float> values(channel.values);
    const std::size_t width = channel.width;
    const bool before = time < times.front() || (arriving && time == times.front());
    const bool after = time > times.back() || (!arriving && time == times.back());
    if (times.size() == 1 || before || after) {
        const std::size_t key = before ? 0 : times.size() - 1;
        std::ranges::copy(values.subspan((key * width * 3) + width, width), value.begin());
        std::ranges::fill(slope, 0.0F);
        return;
    }
    std::size_t key = key_at(times, time, 0);
    // Arriving at a key's time is the end of the stretch before it.
    if (arriving && time == times[key]) {
        --key;
    }
    const float gap = times[key + 1] - times[key];
    const float s = (time - times[key]) / gap;
    const float s2 = s * s;
    const float s3 = s2 * s;
    const std::span<const float> from = values.subspan(key * width * 3, width * 3);
    const std::span<const float> to = values.subspan((key + 1) * width * 3, width * 3);
    for (std::size_t i = 0; i < width; ++i) {
        const float from_value = from[width + i];
        const float from_out = from[(2 * width) + i];
        const float to_value = to[width + i];
        const float to_in = to[i];
        value[i] = (((2.0F * s3) - (3.0F * s2) + 1.0F) * from_value) +
                   ((s3 - (2.0F * s2) + s) * gap * from_out) +
                   (((-2.0F * s3) + (3.0F * s2)) * to_value) + ((s3 - s2) * gap * to_in);
        slope[i] = ((((6.0F * s2) - (6.0F * s)) / gap) * (from_value - to_value)) +
                   (((3.0F * s2) - (4.0F * s) + 1.0F) * from_out) +
                   (((3.0F * s2) - (2.0F * s)) * to_in);
    }
}

/// Adds a key at `time` to `cut`, of a non-CUBIC channel's value at `at` in its clip.
void add_sampled(channel_t& cut, const channel_t& channel, float at, float time) {
    std::vector<float> value(channel.width);
    std::size_t cursor = 0;
    sample(channel, at, cursor, value);
    cut.times.push_back(time);
    cut.values.insert(cut.values.end(), value.begin(), value.end());
}

/// Adds a key at `time` to `cut`, of a CUBIC channel's curve at `at`, arriving or leaving there.
void add_curve(channel_t& cut, const channel_t& channel, float at, float time, bool arriving) {
    std::vector<float> value(channel.width);
    std::vector<float> slope(channel.width);
    curve_at(channel, at, arriving, value, slope);
    cut.times.push_back(time);
    cut.values.insert(cut.values.end(), slope.begin(), slope.end());
    cut.values.insert(cut.values.end(), value.begin(), value.end());
    cut.values.insert(cut.values.end(), slope.begin(), slope.end());
}

channel_t slice_channel(const channel_t& channel, float start, float end) {
    channel_t cut;
    cut.node = channel.node;
    cut.property = channel.property;
    cut.interpolation = channel.interpolation;
    cut.width = channel.width;
    const bool curved = channel.interpolation == interpolation_t::CUBIC;
    const std::size_t width = channel.width;
    const std::size_t stride = curved ? width * 3 : width;
    if (curved) {
        add_curve(cut, channel, start, 0.0F, false);
    } else {
        add_sampled(cut, channel, start, 0.0F);
    }
    for (std::size_t key = 0; key < channel.times.size(); ++key) {
        const float time = channel.times[key];
        if (time <= start || time >= end) {
            continue;
        }
        cut.times.push_back(time - start);
        const auto from = channel.values.begin() + static_cast<std::ptrdiff_t>(key * stride);
        cut.values.insert(cut.values.end(), from, from + static_cast<std::ptrdiff_t>(stride));
        if (!curved) {
            continue;
        }
        // Next to a held end, where the cut's key has no slope, a tangent that was never used
        // would be: it's held flat instead, as the clip was there.
        const std::size_t at = cut.values.size() - stride;
        if (key == 0 && start < time) {
            std::fill_n(cut.values.begin() + static_cast<std::ptrdiff_t>(at), width, 0.0F);
        }
        if (key + 1 == channel.times.size() && end > time) {
            std::fill_n(cut.values.begin() + static_cast<std::ptrdiff_t>(at + (2 * width)), width,
                        0.0F);
        }
    }
    if (end > start) {
        if (curved) {
            add_curve(cut, channel, end, end - start, true);
        } else {
            add_sampled(cut, channel, end, end - start);
        }
    }
    return cut;
}

} // namespace

clip_t slice(const clip_t& clip, float start, float end) {
    clip_t made;
    made.name = clip.name;
    made.nodes = clip.nodes;
    made.frame_rate = clip.frame_rate;
    made.length = end - start;
    for (const channel_t& channel : clip.channels) {
        made.channels.push_back(slice_channel(channel, start, end));
    }
    for (const event_t& event : clip.events) {
        if (event.time >= start && event.time <= end) {
            made.events.push_back({ .time = event.time - start, .name = event.name });
        }
    }
    return made;
}

} // namespace fbe::anim
