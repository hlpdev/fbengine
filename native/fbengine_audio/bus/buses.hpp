#pragma once

// Buses: what channels play into, each a group in the mix with its volume and effects. A bus keeps
// its settings while the audio's stopped, and gets its nodes again when it starts.

#include <cstdint>
#include <memory>
#include <miniaudio.h>

namespace fbe::audio {

struct bus_t;

/// The bus `handle` is for, or null for 0 or one that's gone.
std::shared_ptr<bus_t> find_bus(std::uint64_t handle);

/// The node a channel on `bus` plays into: its group, or the mix's endpoint for none.
ma_node* bus_input(bus_t* bus);

/// Makes every bus's nodes once the mix has started; false, with fbe_audio_error's message, if
/// miniaudio can't.
bool start_buses();

/// Lets every bus's nodes go, after the channels' and before the mix goes.
void stop_buses();

/// What the mixing thread does before each mix of `mix`: the changes to effects asked for since
/// the last.
void update_buses(const ma_engine* mix);

} // namespace fbe::audio
