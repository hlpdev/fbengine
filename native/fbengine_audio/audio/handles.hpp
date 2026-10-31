#pragma once

// How a handle is laid out, as architecture.md has it: a 32-bit slot, then a 24-bit generation,
// then an 8-bit kind, 0 for none. fbengine_audio's kinds are 64 to 95. This stands in for
// native/common/handles.hpp, the one list of every package's kinds, until it's on master; its
// layout is the same, and fbengine_audio takes it then.

#include <cstdint>

namespace fbe::audio {

/// fbengine_audio's kinds of handle, from 64 to 95.
enum class kind_t : std::uint8_t {
    SOUND = 64,
    CHANNEL = 65,
    BUS = 66,
    EMITTER = 67,
};

/// The most a generation is; a slot whose generation would go past it is retired.
constexpr std::uint32_t most_generation = (1U << 24U) - 1U;

constexpr std::uint64_t make_handle(kind_t kind, std::uint32_t generation, std::uint32_t slot) {
    return (static_cast<std::uint64_t>(kind) << 56U) |
           (static_cast<std::uint64_t>(generation & most_generation) << 32U) | slot;
}

constexpr std::uint32_t handle_slot(std::uint64_t handle) {
    return static_cast<std::uint32_t>(handle);
}

constexpr std::uint32_t handle_generation(std::uint64_t handle) {
    return static_cast<std::uint32_t>(handle >> 32U) & most_generation;
}

constexpr kind_t handle_kind(std::uint64_t handle) {
    return static_cast<kind_t>(handle >> 56U);
}

} // namespace fbe::audio
