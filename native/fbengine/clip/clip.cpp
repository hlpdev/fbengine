// Clips' C API: made in code a channel at a time, shared by their handles and the players playing
// them, and sampled.

#include "clip/clip.hpp"

#include "assets/loads.hpp"
#include "assets/model.hpp"
#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace fbe::anim {

namespace {

struct clips_t {
    handle_slots_t slots{ CLIP_KIND };
    std::vector<kept_clip_t> kept;
};

clips_t& clips() {
    static clips_t made;
    return made;
}

/// How wide a property's values are, or 0 for weights, which are as wide as they're given.
std::uint32_t width_of(property_t property) {
    switch (property) {
    case property_t::TRANSLATION:
    case property_t::SCALE:
        return 3;
    case property_t::ROTATION:
        return 4;
    case property_t::WEIGHTS:
        break;
    }
    return 0;
}

const char* name_of(property_t property) {
    switch (property) {
    case property_t::TRANSLATION:
        return "position";
    case property_t::ROTATION:
        return "rotation";
    case property_t::SCALE:
        return "scale";
    case property_t::WEIGHTS:
        break;
    }
    return "weights";
}

/// Whether a channel's keys are fit to sample: times from 0, finite, each after the last, and
/// values finite; false with fbe_error saying what's wrong.
bool fit(std::span<const float> times, std::span<const float> values) {
    for (std::size_t i = 0; i < times.size(); ++i) {
        if (!std::isfinite(times[i]) || times[i] < 0.0F) {
            return fail(std::format("a key's time is 0 seconds or more, not {}", times[i]));
        }
        if (i > 0 && times[i] <= times[i - 1]) {
            return fail(std::format("each key's time is after the last: {} isn't after {}",
                                    times[i], times[i - 1]));
        }
    }
    if (!std::ranges::all_of(values, [](float value) { return std::isfinite(value); })) {
        return fail("a key's values are numbers");
    }
    return true;
}

/// A new clip, held once, as its handle is.
std::uint32_t make_clip() {
    auto& made = clips();
    const std::uint32_t slot = made.slots.make();
    if (slot == made.kept.size()) {
        made.kept.emplace_back();
    } else {
        made.kept[slot] = {};
    }
    made.kept[slot].references = 1;
    return slot;
}

static_assert(static_cast<int>(assets::property_t::WEIGHTS) ==
              static_cast<int>(property_t::WEIGHTS));
static_assert(static_cast<int>(assets::interpolation_t::CUBIC) ==
              static_cast<int>(interpolation_t::CUBIC));

/// Whether a model's channel is one a clip made in code could be: a node the model has, its
/// property's width, a key or more, as many values as its keys take, and fit to sample; false with
/// fbe_error saying what's wrong.
bool fits(const assets::model_t& model, const assets::channel_t& channel, std::size_t index) {
    const auto property = static_cast<property_t>(channel.property);
    const std::uint32_t wide = width_of(property);
    const std::size_t keys = channel.times.size();
    const std::size_t floats =
        keys * channel.width * (channel.interpolation == assets::interpolation_t::CUBIC ? 3 : 1);
    if (channel.node >= model.nodes.size() ||
        (wide != 0 ? channel.width != wide : channel.width < 1) || keys < 1 ||
        channel.values.size() != floats) {
        return fail(std::format("the clip's channel {} has no node, or isn't as wide as its {}, or "
                                "has no keys, or as many values as they take",
                                index, name_of(property)));
    }
    return fit(channel.times, channel.values);
}

/// A model's clip as the clip area keeps it: each channel's node by its name, the names each once.
clip_t clip_of(const assets::model_t& model, const assets::clip_t& from) {
    clip_t made;
    made.name = from.name;
    made.length = from.length;
    made.frame_rate = from.frame_rate;
    for (const assets::channel_t& channel : from.channels) {
        const std::string& named = model.nodes[channel.node].name;
        const auto at = std::ranges::find(made.nodes, named);
        channel_t& to = made.channels.emplace_back();
        to.node = static_cast<std::uint32_t>(at - made.nodes.begin());
        if (at == made.nodes.end()) {
            made.nodes.push_back(named);
        }
        to.property = static_cast<property_t>(channel.property);
        to.interpolation = static_cast<interpolation_t>(channel.interpolation);
        to.width = channel.width;
        to.times = channel.times;
        to.values = channel.values;
    }
    return made;
}

} // namespace

std::uint32_t clip_slot(std::uint64_t handle) {
    return scene::live_slot(clips().slots, handle, "clip");
}

kept_clip_t& kept_clip(std::uint32_t slot) {
    return clips().kept[slot];
}

std::uint64_t clip_handle(std::uint32_t slot) {
    return clips().slots.handle(slot);
}

void retain_clip(std::uint32_t slot) {
    ++clips().kept[slot].references;
}

void release_clip(std::uint32_t slot) {
    kept_clip_t& found = clips().kept[slot];
    if (--found.references == 0) {
        found = {};
        clips().slots.free(slot);
    }
}

} // namespace fbe::anim

using fbe::fail;
using fbe::anim::clip_slot;
using fbe::anim::clips;
using fbe::anim::interpolation_t;
using fbe::anim::kept_clip;
using fbe::anim::kept_clip_t;
using fbe::anim::property_t;
using fbe::scene::none;

extern "C" {

FBE_API uint64_t fbe_clip_create(const char* name, float frame_rate) {
    if (!std::isfinite(frame_rate) || frame_rate < 0.0F) {
        fail(std::format("a clip's frames a second are 0 or more, not {}", frame_rate));
        return 0;
    }
    const std::uint32_t slot = fbe::anim::make_clip();
    kept_clip_t& clip = kept_clip(slot);
    clip.clip.name = name != nullptr ? name : "";
    clip.clip.frame_rate = frame_rate;
    return clips().slots.handle(slot);
}

FBE_API uint64_t fbe_clip_from_model(uint64_t load, int32_t index) {
    const fbe::assets::load_t* found = fbe::assets::loads().get(load);
    if (found == nullptr || !found->done || found->error ||
        found->kind != fbe::assets::load_kind_t::MODEL) {
        fail("the load isn't a model load done and loaded");
        return 0;
    }
    const std::vector<fbe::assets::clip_t>& from = found->model.clips;
    if (index < 0 || std::cmp_greater_equal(index, from.size())) {
        fail(std::format("the model has {} clips; there's no clip {}", from.size(), index));
        return 0;
    }
    const fbe::assets::clip_t& clip = from[static_cast<std::size_t>(index)];
    for (std::size_t i = 0; i < clip.channels.size(); ++i) {
        if (!fbe::anim::fits(found->model, clip.channels[i], i)) {
            return 0;
        }
    }
    try {
        fbe::anim::clip_t made = fbe::anim::clip_of(found->model, clip);
        const std::uint32_t slot = fbe::anim::make_clip();
        kept_clip(slot).clip = std::move(made);
        return clips().slots.handle(slot);
    } catch (const std::bad_alloc&) {
        fail("out of memory");
        return 0;
    }
}

FBE_API bool fbe_clip_retain(uint64_t clip) {
    const std::uint32_t slot = clip_slot(clip);
    if (slot == none) {
        return false;
    }
    fbe::anim::retain_clip(slot);
    return true;
}

FBE_API bool fbe_clip_release(uint64_t clip) {
    const std::uint32_t slot = clip_slot(clip);
    if (slot == none) {
        return false;
    }
    fbe::anim::release_clip(slot);
    return true;
}

FBE_API bool fbe_clip_alive(uint64_t clip) {
    return clips().slots.find(clip).has_value();
}

FBE_API bool fbe_clip_add_channel(uint64_t clip, const char* node, int32_t property,
                                  int32_t interpolation, int32_t width, const float* times,
                                  const float* values, int32_t keys) {
    const std::uint32_t slot = clip_slot(clip);
    if (slot == none) {
        return false;
    }
    if (property < FBE_CLIP_TRANSLATION || property > FBE_CLIP_WEIGHTS) {
        return fail(std::format("{} isn't a property a clip moves", property));
    }
    if (interpolation < FBE_CLIP_STEP || interpolation > FBE_CLIP_CUBIC) {
        return fail(std::format("{} isn't an interpolation", interpolation));
    }
    if (keys < 1 || times == nullptr || values == nullptr) {
        return fail("a channel has a key or more");
    }
    const auto moves = static_cast<property_t>(property);
    const std::uint32_t wide = fbe::anim::width_of(moves);
    if (wide == 0 && width < 1) {
        return fail(std::format("a channel's weights are 1 or more floats a key, not {}", width));
    }
    if (wide != 0 && std::cmp_not_equal(width, wide)) {
        return fail(std::format("a channel's {} is {} floats a key, not {}",
                                fbe::anim::name_of(moves), wide, width));
    }
    const auto goes = static_cast<interpolation_t>(interpolation);
    const auto count = static_cast<std::size_t>(keys);
    const std::size_t floats =
        count * static_cast<std::size_t>(width) * (goes == interpolation_t::CUBIC ? 3 : 1);
    const std::span<const float> key_times(times, count);
    const std::span<const float> key_values(values, floats);
    if (!fbe::anim::fit(key_times, key_values)) {
        return false;
    }
    fbe::anim::clip_t& found = kept_clip(slot).clip;
    const std::string named = node != nullptr ? node : "";
    auto at = std::ranges::find(found.nodes, named);
    const auto index = static_cast<std::uint32_t>(at - found.nodes.begin());
    const bool moved = std::ranges::any_of(found.channels, [&](const fbe::anim::channel_t& was) {
        return was.node == index && was.property == moves;
    });
    if (at != found.nodes.end() && moved) {
        return fail(std::format("the clip moves the {} of \"{}\" already",
                                fbe::anim::name_of(moves), named));
    }
    if (at == found.nodes.end()) {
        found.nodes.push_back(named);
    }
    fbe::anim::channel_t& made = found.channels.emplace_back();
    made.node = index;
    made.property = moves;
    made.interpolation = goes;
    made.width = static_cast<std::uint32_t>(width);
    made.times.assign(key_times.begin(), key_times.end());
    made.values.assign(key_values.begin(), key_values.end());
    found.length = std::max(found.length, key_times.back());
    ++kept_clip(slot).version;
    return true;
}

FBE_API const char* fbe_clip_name(uint64_t clip) {
    const std::uint32_t slot = clip_slot(clip);
    return slot != none ? kept_clip(slot).clip.name.c_str() : "";
}

FBE_API float fbe_clip_length(uint64_t clip) {
    const std::uint32_t slot = clip_slot(clip);
    return slot != none ? kept_clip(slot).clip.length : 0.0F;
}

FBE_API float fbe_clip_frame_rate(uint64_t clip) {
    const std::uint32_t slot = clip_slot(clip);
    return slot != none ? kept_clip(slot).clip.frame_rate : 0.0F;
}

FBE_API int32_t fbe_clip_channel_count(uint64_t clip) {
    const std::uint32_t slot = clip_slot(clip);
    return slot != none ? static_cast<int32_t>(kept_clip(slot).clip.channels.size()) : 0;
}

FBE_API int32_t fbe_clip_sample(uint64_t clip, int32_t channel, float time, float* out,
                                int32_t capacity) {
    const std::uint32_t slot = clip_slot(clip);
    if (slot == none) {
        return -1;
    }
    const std::vector<fbe::anim::channel_t>& channels = kept_clip(slot).clip.channels;
    if (channel < 0 || static_cast<std::size_t>(channel) >= channels.size()) {
        fail(std::format("the clip has {} channels; there's no channel {}", channels.size(),
                         channel));
        return -1;
    }
    const fbe::anim::channel_t& found = channels[static_cast<std::size_t>(channel)];
    if (out == nullptr || std::cmp_less(capacity, found.width)) {
        fail(std::format("a sample of the channel is {} floats", found.width));
        return -1;
    }
    if (!std::isfinite(time)) {
        fail(std::format("a clip is sampled at a time that's a number, not {}", time));
        return -1;
    }
    std::size_t cursor = 0;
    fbe::anim::sample(found, time, cursor, std::span<float>(out, found.width));
    return static_cast<int32_t>(found.width);
}

} // extern "C"
