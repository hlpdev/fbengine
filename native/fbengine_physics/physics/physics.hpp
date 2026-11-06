#pragma once

// What fbengine_physics_native's own files share: its errors, Jolt, which starts the first time a
// world or a shape needs it, and the worlds, with their layers, contacts, bodies, joints, and
// characters.

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on

#include "physics/fbe_physics.h"
#include "physics/handles.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <Jolt/Core/FPControlWord.h>
#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorder.h>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace fbe::physics {

/// The floating point mode a world's steps, its characters' moves, its kinematic bodies' moves,
/// and its casts run in, on the thread that calls them and on Jolt's: rounding to the nearest, and
/// numbers too small for a float kept, not flushed to 0, whatever the game, or a library it loads,
/// has set, so they come out the same. It's put back as it goes.
#ifdef JPH_USE_SSE
// MXCSR's rounding, bits 13 and 14, flush to zero, bit 15, and denormals are zero, bit 6.
using fp_mode_t = JPH::FPControlWord<0x0000U, 0x6000U | 0x8000U | 0x0040U>;
#elif defined(JPH_CPU_ARM) && defined(JPH_COMPILER_MSVC)
using fp_mode_t = JPH::FPControlWord<_RC_NEAR | _DN_SAVE, _MCW_RC | _MCW_DN>;
#elif defined(JPH_CPU_ARM) && defined(JPH_USE_NEON)
// FPCR's rounding, bits 22 and 23, and flush to zero, bit 24.
using fp_mode_t = JPH::FPControlWord<0, (JPH::uint64{ 3 } << 22U) | (JPH::uint64{ 1 } << 24U)>;
#else
#error "fbengine_physics sets the floating point mode of SSE or of NEON"
#endif

/// A body's layer, 0 to 31, and whether it moves, as one of Jolt's object layers, which keep
/// what doesn't move apart in Jolt's broad phase.
constexpr JPH::ObjectLayer object_layer(std::uint32_t layer, bool moves) {
    return static_cast<JPH::ObjectLayer>((layer * 2U) + (moves ? 1U : 0U));
}

/// The layer an object layer is of.
constexpr std::uint32_t layer_of(JPH::ObjectLayer layer) {
    return layer >> 1U;
}

/// Whether an object layer is of what moves.
constexpr bool moves(JPH::ObjectLayer layer) {
    return (layer & 1U) != 0;
}

/// Which of a world's layers collide with which: all with all at first, but what doesn't move
/// with itself. The game changes it between steps.
class layers_t final : public JPH::ObjectLayerPairFilter {
public:
    layers_t();

    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override;

    /// Whether `a` and `b` collide, both 0 to 31.
    [[nodiscard]] bool collides(std::uint32_t a, std::uint32_t b) const;
    void set_collides(std::uint32_t a, std::uint32_t b, bool collides);

private:
    // A bit for each layer each collides with.
    std::array<std::uint32_t, FBE_PHYSICS_LAYERS> m_collides{};
};

/// Jolt's broad phase layers: what doesn't move, and what does.
class broad_phase_t final : public JPH::BroadPhaseLayerInterface {
public:
    [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override;
    [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override;
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    [[nodiscard]] const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override;
#endif
};

/// What doesn't move is never tested against itself.
class object_vs_broad_phase_t final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer,
                                     JPH::BroadPhaseLayer broad_phase) const override;
};

/// A contact Jolt found or lost, on one of its threads, kept for the step's end.
struct contact_event_t {
    bool added;
    JPH::BodyID a;
    JPH::BodyID b;
    JPH::SubShapeID a_part;
    JPH::SubShapeID b_part;
    std::uint64_t a_handle;
    std::uint64_t b_handle;
    JPH::Vec3 position;
    // From a to b.
    JPH::Vec3 normal;
    float speed;
};

/// What Jolt's threads say of contacts during a step.
class contacts_t final : public JPH::ContactListener {
public:
    void OnContactAdded(const JPH::Body& a, const JPH::Body& b,
                        const JPH::ContactManifold& manifold,
                        JPH::ContactSettings& settings) override;
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override;

    /// The step's events, which the world takes once it's stepped.
    std::vector<contact_event_t> take();

private:
    std::mutex m_lock;
    std::vector<contact_event_t> m_events;
};

/// Two bodies touching: the pairs of their parts that touch, each kept while the bodies sleep,
/// when Jolt forgets it, and their handles and ids.
struct touching_t {
    // By the parts' ids; true for a pair kept while the bodies sleep.
    std::map<std::uint64_t, bool> parts;
    std::uint64_t a;
    std::uint64_t b;
    JPH::BodyID a_id;
    JPH::BodyID b_id;
    // Whether either was awake as the step began, so a pair kept that it didn't find again is gone.
    bool awake = false;
};

/// A world: Jolt's physics system, and what it's made with.
struct world_t {
    explicit world_t(std::uint32_t max_bodies);
    ~world_t();
    world_t(const world_t&) = delete;
    world_t& operator=(const world_t&) = delete;
    world_t(world_t&&) = delete;
    world_t& operator=(world_t&&) = delete;

    layers_t layers;
    broad_phase_t broad_phase;
    object_vs_broad_phase_t object_vs_broad_phase;
    contacts_t contact_listener;
    JPH::PhysicsSystem system;
    // Characters collide with each other through it.
    JPH::CharacterVsCharacterCollisionSimple characters_vs;
    // Its bodies', joints', and characters' handles, in no order.
    std::vector<std::uint64_t> bodies;
    std::vector<std::uint64_t> joints;
    std::vector<std::uint64_t> characters;
    // The bodies touching, by their Jolt ids', in order, so their events come in order.
    std::map<std::uint64_t, touching_t> touching;
    // What began and ended in the last step.
    std::vector<fbe_physics_contact_t> began;
    std::vector<std::uint64_t> ended;
    // How many pairs of bodies touching, and manifolds of their contacts, Jolt has room for.
    std::uint32_t most_pairs = 0;
    std::uint32_t most_manifolds = 0;
    std::int64_t steps = 0;
    // How many times something's moved its bodies: a step, or a call that puts one somewhere.
    std::int64_t moves = 0;
    bool overflowed = false;
    // Bodies added and taken out since the last step or optimize, which Jolt's broad phase keeps
    // room for until one.
    std::uint32_t changes = 0;
    // The state it was last saved in, which fbe_physics_saved copies out.
    std::vector<std::uint8_t> saved;
};

/// The most a coordinate of where a body, a character, or a joint's point is goes from 0: well
/// within the 1e15 meters Jolt's broad phase keeps what's in it to, with the largest shape there.
constexpr float most_position = 1.0e9F;

/// The most a shape's sizes, or a coordinate of its points or of its parts, go from 0: 100 km,
/// past which a body of it weighs, and turns, by more than a float holds.
constexpr float most_length = 1.0e5F;

/// The most a coordinate of a body's velocity, or of what its forces and torques would change it
/// by in a second, goes from 0: far past Jolt's most, but with room in a float to square it, and to
/// turn it by a lever as long as a shape.
constexpr float most_motion = 1.0e12F;

/// The most a coordinate of gravity, or a body's gravity factor, goes from 0, so the two make no
/// more than most_motion.
constexpr float most_gravity = 1.0e6F;

/// The most a coordinate of a character's velocity goes from 0, so a move of a second takes it a
/// thousandth of the way to most_position.
constexpr float most_speed = 1.0e6F;

/// Whether `x`, `y`, and `z` are each at most `most` from 0, as no NaN is.
inline bool within(float x, float y, float z, float most) {
    return std::abs(x) <= most && std::abs(y) <= most && std::abs(z) <= most;
}

inline bool within(JPH::Vec3Arg v, float most) {
    return within(v.GetX(), v.GetY(), v.GetZ(), most);
}

/// Whether `body`'s velocities, and what its forces and torques would change them by in a second,
/// are within most_motion; true for a static body.
bool moving_within(const JPH::Body& body);

/// How much an impulse on `body` changes its velocities by, at most, by its inverse mass and
/// inertia: 0 for a body that only moves as it's moved, or none.
float response(const JPH::Body* body);

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

/// Frees `world`'s bodies, but its characters'.
void free_bodies(world_t& world);

/// The Jolt body `handle` is, and the world it's in, or nullptr, with fbe_physics_error() saying
/// so, for what isn't a body.
JPH::Body* find_body(std::uint64_t handle, world_t*& world);

/// The handle of the body a character's Jolt body is, which `world` keeps until the character's
/// freed, when drop_character_body frees it.
std::uint64_t add_character_body(world_t& world, JPH::BodyID id, float padding);
void drop_character_body(std::uint64_t handle);

/// The shape `handle` is, or nullptr, with fbe_physics_error() saying so, for one that's freed or
/// never was.
const JPH::Shape* find_shape(std::uint64_t handle);

/// Frees `world`'s joints, or those of its `body`, before its bodies go.
void free_joints(world_t& world);
void free_joints_of(const world_t& world, std::uint64_t body);

/// Frees `world`'s characters, and their bodies.
void free_characters(world_t& world);

/// Whether reading `state` has run out, which StateRecorder's two streams both say.
inline bool failed(const JPH::StateRecorder& state) {
    return static_cast<const JPH::StreamIn&>(state).IsFailed();
}

/// Saves `world`'s characters' states into `state`, in their handles' order, or restores them from
/// it; false if it isn't of the same characters, or ends too soon.
void save_characters(const world_t& world, JPH::StateRecorder& state);
void hash_characters(const world_t& world, JPH::StateRecorder& state);
bool restore_characters(const world_t& world, JPH::StateRecorder& state);

/// Notes, as a step begins, which of the pairs `world` keeps while their bodies sleep are awake.
void before_contacts(world_t& world);

/// Turns the contacts Jolt found and lost in a step into `world`'s began and ended.
void take_contacts(world_t& world);

/// Forgets what `body` touches, as it goes: it ends with no event.
void forget_contacts(world_t& world, JPH::BodyID body);

/// The triangle of a mesh `part` of `body` is, -1 if it isn't a mesh's.
std::int32_t triangle_of(const JPH::Body& body, const JPH::SubShapeID& part);

/// Jolt's job system, which steps worlds, started with Jolt.
JPH::JobSystem& jobs();

/// The memory a step uses while it runs.
JPH::TempAllocator& step_memory();

} // namespace fbe::physics
