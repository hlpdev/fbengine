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
    // Bodies added and taken out since the last step or optimize, which Jolt's broad phase keeps
    // room for until one.
    std::uint32_t changes = 0;
};

/// Starts Jolt, its allocator, its types, and its job system, if they haven't started: before
/// anything of Jolt's is made.
void start();

/// `x`, `y`, `z`, and `w` normalized, worked out in doubles so a quat too long or too short for a
/// float's square still is; false, saying so, if they aren't finite, or are 0.
bool normalized(float x, float y, float z, float w, JPH::Quat& made);

/// Keeps `message` for fbe_physics_error(), and returns false, for a call that failed.
bool fail(std::string_view message);

/// The world `handle` is, or nullptr, with fbe_physics_error() saying so, for one that's freed or
/// never was.
world_t* find_world(std::uint64_t handle);

/// Counts a body added to or taken out of `world`, sorting its broad phase once there have been so
/// many since its last step that it would run out of room.
void changed(world_t& world);

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
