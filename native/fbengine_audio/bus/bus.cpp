// Buses: each a miniaudio sound group, then its low-pass and reverb nodes when it has them, into
// its parent's group or the mix's endpoint. The main thread makes, joins, and lets go of nodes,
// which miniaudio lets it do while the mix runs; a change to a node in the mix, a new cutoff or
// reverb, waits for the mixing thread, which update_buses makes before each mix.

#include "audio/mixer.hpp"
#include "audio/pool.hpp"
#include "bus/buses.hpp"
#include "channel/voices.hpp"
#include "fbengine_audio.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <extras/nodes/ma_reverb_node/ma_reverb_node.h>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace {

/// A reverb's settings, each 0 to 1.
struct reverb_t {
    float room_size = 0.0F;
    float damping = 0.0F;
    float width = 0.0F;
    float wet = 0.0F;
    float dry = 0.0F;
};

} // namespace

namespace fbe::audio {

/// A bus. Its settings are the main thread's, which the mixing thread reads under the lock; its
/// nodes, while the audio's started, are made and let go on the main thread, and the mixing thread
/// changes a low-pass's or reverb's settings, under the lock, when they're marked changed.
/// miniaudio's init functions clear each of its structs first.
struct bus_t {
    std::shared_ptr<bus_t> parent;
    float volume = 1.0F;
    float cutoff = 0.0F;
    std::optional<reverb_t> reverb;
    ma_sound_group group;
    ma_lpf_node low_pass;
    ma_reverb_node reverb_node;
    bool has_group = false;
    bool has_low_pass = false;
    bool has_reverb = false;
    bool cutoff_changed = false;
    bool reverb_changed = false;

    bus_t() = default;
    bus_t(const bus_t&) = delete;
    bus_t& operator=(const bus_t&) = delete;
    bus_t(bus_t&&) = delete;
    bus_t& operator=(bus_t&&) = delete;

    ~bus_t();
};

} // namespace fbe::audio

namespace {

using fbe::audio::bus_t;

constexpr ma_uint32 low_pass_order = 4;
constexpr float lowest_cutoff = 10.0F;
constexpr float highest_cutoff = 24000.0F;
/// Where a low-pass is held below half the mix's rate, past which its filter means nothing.
constexpr double most_of_half = 0.9;
/// The channels and rates Freeverb, as verblib has it, takes.
constexpr ma_uint32 most_reverb_channels = 2;
/// A group at the mix's rate needs no resampler, which would hold a frame, nor a place.
constexpr ma_uint32 group_flags = static_cast<ma_uint32>(MA_SOUND_FLAG_NO_PITCH) |
                                  static_cast<ma_uint32>(MA_SOUND_FLAG_NO_SPATIALIZATION);

/// Every bus something has, by handle or as a channel's or another bus's, and the handles: made
/// once and never let go, as a program can end with the audio started.
struct buses_t {
    std::mutex lock;
    std::vector<bus_t*> all;
    fbe::audio::pool_t<std::shared_ptr<bus_t>> handles;
};

buses_t& buses() {
    static auto* made = new buses_t();
    return *made;
}

ma_node_graph* graph() {
    return ma_engine_get_node_graph(fbe::audio::engine());
}

/// A low-pass at `cutoff` in `mix`, held below half its rate.
ma_lpf_node_config low_pass_config(const ma_engine* mix, float cutoff) {
    const ma_uint32 rate = ma_engine_get_sample_rate(mix);
    const double held = std::min(static_cast<double>(cutoff), rate * 0.5 * most_of_half);
    return ma_lpf_node_config_init(ma_engine_get_channels(mix), rate, held, low_pass_order);
}

/// Gives `bus`'s reverb node `settings`, verblib's dry being twice what goes in.
void apply_reverb(bus_t& bus, const reverb_t& settings) {
    verblib* reverb = &bus.reverb_node.reverb;
    verblib_set_room_size(reverb, settings.room_size);
    verblib_set_damping(reverb, settings.damping);
    verblib_set_width(reverb, settings.width);
    verblib_set_wet(reverb, settings.wet);
    verblib_set_dry(reverb, settings.dry / verblib_scaledry);
}

bool make_low_pass(bus_t& bus) {
    const ma_lpf_node_config config = low_pass_config(fbe::audio::engine(), bus.cutoff);
    const ma_result result = ma_lpf_node_init(graph(), &config, nullptr, &bus.low_pass);
    if (result != MA_SUCCESS) {
        return fbe::audio::fail_ma("couldn't make the bus's low-pass", result);
    }
    return true;
}

/// Makes `bus`'s reverb node, out of the mix until it's joined; false for a mix Freeverb can't do.
bool make_reverb(bus_t& bus) {
    const ma_engine* mix = fbe::audio::engine();
    const ma_uint32 channels = ma_engine_get_channels(mix);
    if (!bus.reverb.has_value() || channels > most_reverb_channels) {
        return false;
    }
    const ma_reverb_node_config config =
        ma_reverb_node_config_init(channels, ma_engine_get_sample_rate(mix));
    if (ma_reverb_node_init(graph(), &config, nullptr, &bus.reverb_node) != MA_SUCCESS) {
        return false;
    }
    apply_reverb(bus, *bus.reverb);
    return true;
}

/// Joins `bus`'s nodes in order, its group through its effects, into its parent's group, or the
/// mix's endpoint.
void join(bus_t& bus, bool low_pass, bool reverb) {
    std::array<ma_node*, 3> chain{};
    std::size_t count = 0;
    chain.at(count++) = &bus.group;
    if (low_pass) {
        chain.at(count++) = &bus.low_pass;
    }
    if (reverb) {
        chain.at(count++) = &bus.reverb_node;
    }
    ma_node* into = bus.parent != nullptr ? static_cast<ma_node*>(&bus.parent->group)
                                          : ma_engine_get_endpoint(fbe::audio::engine());
    for (std::size_t i = 0; i < count; ++i) {
        ma_node_attach_output_bus(chain.at(i), 0, i + 1 < count ? chain.at(i + 1) : into, 0);
    }
}

/// Makes `bus`'s nodes, and its parents' first, in the started mix.
bool build(bus_t& bus) {
    if (bus.has_group) {
        return true;
    }
    if (bus.parent != nullptr && !build(*bus.parent)) {
        return false;
    }
    const ma_result result =
        ma_sound_group_init(fbe::audio::engine(), group_flags, nullptr, &bus.group);
    if (result != MA_SUCCESS) {
        return fbe::audio::fail_ma("couldn't make the bus", result);
    }
    bus.has_group = true;
    ma_sound_group_set_volume(&bus.group, bus.volume);
    const bool low_pass = bus.cutoff > 0.0F && make_low_pass(bus);
    const bool reverb = bus.reverb.has_value() && make_reverb(bus);
    join(bus, low_pass, reverb);
    const std::scoped_lock lock(buses().lock);
    bus.has_low_pass = low_pass;
    bus.has_reverb = reverb;
    return true;
}

/// Lets `bus`'s nodes go, the mixing thread told first.
void tear_down(bus_t& bus) {
    bool low_pass = false;
    bool reverb = false;
    {
        const std::scoped_lock lock(buses().lock);
        low_pass = bus.has_low_pass;
        reverb = bus.has_reverb;
        bus.has_low_pass = false;
        bus.has_reverb = false;
    }
    if (reverb) {
        ma_reverb_node_uninit(&bus.reverb_node, nullptr);
    }
    if (low_pass) {
        ma_lpf_node_uninit(&bus.low_pass, nullptr);
    }
    if (bus.has_group) {
        ma_sound_group_uninit(&bus.group);
        bus.has_group = false;
    }
}

/// The bus `handle` is for, or null, with fbe_audio_error's message, when it's gone.
bus_t* find(std::uint64_t handle) {
    const std::shared_ptr<bus_t>* found = buses().handles.find(handle);
    if (found == nullptr) {
        fbe::audio::fail("the bus was freed");
        return nullptr;
    }
    return found->get();
}

bool check_fraction(float value, const char* what) {
    if (!std::isfinite(value) || value < 0.0F || value > 1.0F) {
        return fbe::audio::fail(std::format("a reverb's {} is 0 to 1, not {}", what, value));
    }
    return true;
}

} // namespace

namespace fbe::audio {

bus_t::~bus_t() {
    {
        const std::scoped_lock lock(buses().lock);
        std::erase(buses().all, this);
    }
    tear_down(*this);
}

std::shared_ptr<bus_t> find_bus(std::uint64_t handle) {
    const std::shared_ptr<bus_t>* found = buses().handles.find(handle);
    return found != nullptr ? *found : nullptr;
}

ma_sound_group* bus_group(bus_t* bus) {
    return bus != nullptr ? &bus->group : nullptr;
}

ma_node* bus_input(bus_t* bus) {
    if (bus != nullptr) {
        return &bus->group;
    }
    return ma_engine_get_endpoint(engine());
}

bool start_buses() {
    std::vector<bus_t*> all;
    {
        const std::scoped_lock lock(buses().lock);
        all = buses().all;
    }
    return std::ranges::all_of(all, [](bus_t* bus) { return build(*bus); });
}

void stop_buses() {
    std::vector<bus_t*> all;
    {
        const std::scoped_lock lock(buses().lock);
        all = buses().all;
    }
    for (bus_t* bus : all) {
        tear_down(*bus);
    }
}

void update_buses(const ma_engine* mix) {
    const std::scoped_lock lock(buses().lock);
    for (bus_t* bus : buses().all) {
        if (bus->cutoff_changed && bus->has_low_pass) {
            const ma_lpf_node_config config = low_pass_config(mix, bus->cutoff);
            ma_lpf_node_reinit(&config.lpf, &bus->low_pass);
        }
        if (bus->reverb_changed && bus->has_reverb && bus->reverb.has_value()) {
            apply_reverb(*bus, *bus->reverb);
        }
        bus->cutoff_changed = false;
        bus->reverb_changed = false;
    }
}

} // namespace fbe::audio

extern "C" {

FBE_AUDIO_API uint64_t fbe_bus_add(uint64_t parent) {
    std::shared_ptr<bus_t> in;
    if (parent != 0) {
        in = fbe::audio::find_bus(parent);
        if (in == nullptr) {
            fbe::audio::fail("the parent bus was freed");
            return 0;
        }
    }
    auto bus = std::make_shared<bus_t>();
    bus->parent = std::move(in);
    {
        const std::scoped_lock lock(buses().lock);
        buses().all.push_back(bus.get());
    }
    if (fbe::audio::engine() != nullptr && !build(*bus)) {
        return 0;
    }
    return buses().handles.add(std::make_unique<std::shared_ptr<bus_t>>(std::move(bus)));
}

FBE_AUDIO_API void fbe_bus_free(uint64_t bus) {
    buses().handles.remove(bus);
}

FBE_AUDIO_API bool fbe_bus_set_volume(uint64_t bus, float volume) {
    if (!fbe::audio::check_volume(volume)) {
        return false;
    }
    bus_t* found = find(bus);
    if (found == nullptr) {
        return false;
    }
    found->volume = volume;
    if (found->has_group) {
        ma_sound_group_set_volume(&found->group, volume);
    }
    return true;
}

FBE_AUDIO_API float fbe_bus_volume(uint64_t bus) {
    const bus_t* found = find(bus);
    return found != nullptr ? found->volume : 0.0F;
}

FBE_AUDIO_API bool fbe_bus_set_low_pass(uint64_t bus, float cutoff) {
    if (cutoff != 0.0F &&
        (!std::isfinite(cutoff) || cutoff < lowest_cutoff || cutoff > highest_cutoff)) {
        return fbe::audio::fail(std::format("a low-pass is 0, for none, or {} to {} Hz, not {}",
                                            lowest_cutoff, highest_cutoff, cutoff));
    }
    bus_t* found = find(bus);
    if (found == nullptr) {
        return false;
    }
    {
        const std::scoped_lock lock(buses().lock);
        found->cutoff = cutoff;
        found->cutoff_changed = true;
    }
    if (!found->has_group) {
        return true;
    }
    if (cutoff > 0.0F && !found->has_low_pass) {
        if (!make_low_pass(*found)) {
            return false;
        }
        join(*found, true, found->has_reverb);
        const std::scoped_lock lock(buses().lock);
        found->has_low_pass = true;
    } else if (cutoff == 0.0F && found->has_low_pass) {
        {
            const std::scoped_lock lock(buses().lock);
            found->has_low_pass = false;
        }
        join(*found, false, found->has_reverb);
        ma_lpf_node_uninit(&found->low_pass, nullptr);
    }
    return true;
}

FBE_AUDIO_API float fbe_bus_low_pass(uint64_t bus) {
    const bus_t* found = find(bus);
    return found != nullptr ? found->cutoff : 0.0F;
}

FBE_AUDIO_API bool fbe_bus_set_reverb(uint64_t bus, float room_size, float damping, float width,
                                      float wet, float dry) {
    if (!check_fraction(room_size, "room size") || !check_fraction(damping, "damping") ||
        !check_fraction(width, "width") || !check_fraction(wet, "wet") ||
        !check_fraction(dry, "dry")) {
        return false;
    }
    bus_t* found = find(bus);
    if (found == nullptr) {
        return false;
    }
    {
        const std::scoped_lock lock(buses().lock);
        found->reverb = reverb_t{
            .room_size = room_size,
            .damping = damping,
            .width = width,
            .wet = wet,
            .dry = dry,
        };
        found->reverb_changed = true;
    }
    if (found->has_group && !found->has_reverb && make_reverb(*found)) {
        join(*found, found->has_low_pass, true);
        const std::scoped_lock lock(buses().lock);
        found->has_reverb = true;
    }
    return true;
}

FBE_AUDIO_API void fbe_bus_clear_reverb(uint64_t bus) {
    const std::shared_ptr<bus_t>* held = buses().handles.find(bus);
    if (held == nullptr) {
        return;
    }
    bus_t* found = held->get();
    bool heard = false;
    {
        const std::scoped_lock lock(buses().lock);
        found->reverb.reset();
        heard = found->has_reverb;
        found->has_reverb = false;
    }
    if (heard) {
        join(*found, found->has_low_pass, false);
        ma_reverb_node_uninit(&found->reverb_node, nullptr);
    }
}

FBE_AUDIO_API bool fbe_bus_has_reverb(uint64_t bus) {
    const bus_t* found = find(bus);
    return found != nullptr && found->reverb.has_value();
}

FBE_AUDIO_API bool fbe_bus_reverb_heard(uint64_t bus) {
    const bus_t* found = find(bus);
    return found != nullptr && found->has_reverb;
}

} // extern "C"
