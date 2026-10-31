#pragma once

// What fbengine_physics_native's own files share: its errors, Jolt, which starts the first time a
// world needs it, and the worlds.

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on

#include "physics/handles.hpp"

#include <cstdint>
#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <memory>
#include <string_view>
#include <vector>

namespace fbe::physics {

/// Jolt's object layers: what doesn't move, and what does, which its broad phase keeps apart.
constexpr JPH::ObjectLayer not_moving_layer = 0;
constexpr JPH::ObjectLayer moving_layer = 1;
constexpr JPH::ObjectLayer object_layer_count = 2;

/// A world: Jolt's physics system, and what it's made with.
struct world_t {
    explicit world_t(std::uint32_t max_bodies);

    JPH::BroadPhaseLayerInterfaceTable broad_phase_layers;
    JPH::ObjectLayerPairFilterTable object_layers;
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> object_vs_broad_phase;
    JPH::PhysicsSystem system;
    // Its bodies' handles, in no order.
    std::vector<std::uint64_t> bodies;
    std::int64_t steps = 0;
    bool overflowed = false;
};

/// Keeps `message` for fbe_physics_error(), and returns false, for a call that failed.
bool fail(std::string_view message);

/// The world `handle` is, or nullptr, with fbe_physics_error() saying so, for one that's freed or
/// never was.
world_t* find_world(std::uint64_t handle);

/// Frees `world`'s bodies.
void free_bodies(world_t& world);

/// The shape `handle` is, or nullptr, with fbe_physics_error() saying so, for one that's freed or
/// never was.
const JPH::Shape* find_shape(std::uint64_t handle);

/// Jolt's job system, which steps worlds, started with Jolt.
JPH::JobSystem& jobs();

/// The memory a step uses while it runs.
JPH::TempAllocator& step_memory();

} // namespace fbe::physics
