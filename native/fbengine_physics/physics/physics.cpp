// Worlds: Jolt's physics systems, by handle, and Jolt itself, which starts with the first world
// and stays until the process ends, its job system's threads sleeping while no world steps.

#include "physics/physics.hpp"

#include "fbengine_physics.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <format>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/RegisterTypes.h>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace fbe::physics;

thread_local std::string last_error;

// Jolt's broad phase layers, one for each object layer.
constexpr JPH::BroadPhaseLayer not_moving_broad_phase{ 0 };
constexpr JPH::BroadPhaseLayer moving_broad_phase{ 1 };
constexpr std::uint32_t broad_phase_layer_count = 2;

// What a step's jobs can be, as Jolt's samples have them.
constexpr std::uint32_t max_jobs = 2048;
constexpr std::uint32_t max_barriers = 64;
// The most collision steps one of Jolt's updates takes, all of whose jobs it makes at its start:
// at Jolt's widest, 32 at once, a collision step makes up to 300 of the 2048, so 4 leave room for
// the jobs a step makes as it goes. More are taken in several updates, the same on every machine.
constexpr std::int32_t collision_steps_an_update = 4;
// A step's memory, past which it takes from the heap.
constexpr std::uint32_t step_memory_size = 16U * 1024U * 1024U;
// The fewest pairs of touching bodies, and contacts between them, a world keeps room for; past
// that, a pair for each body it's made for, and a contact for every other.
constexpr std::uint32_t fewest_pairs = 1024;

// The threads Jolt's job system is to have, -1 for a core's less one.
std::int32_t threads_wanted = -1;

std::int32_t thread_count() {
    if (threads_wanted >= 0) {
        return threads_wanted;
    }
    const auto cores = static_cast<std::int32_t>(std::max(std::thread::hardware_concurrency(), 1U));
    return std::min(cores - 1, FBE_PHYSICS_MAX_THREADS);
}

// Jolt's warnings, which it says rarely, as a step running out of room does, through a printf of
// its own.
void trace(const char* format, ...) { // NOLINT(modernize-avoid-variadic-functions)
    std::va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
    std::fputc('\n', stderr);
}

#ifdef JPH_ENABLE_ASSERTS
// A debug build's: a call Jolt doesn't take is this DLL's mistake, which stops the program; but a
// step that ran out of room for contacts is the world's, which fbe_physics_overflowed says.
bool assert_failed(const char* expression, const char* message, const char* file,
                   unsigned int line) {
    if (std::string_view(expression) == "errors == EPhysicsUpdateError::None") {
        return false;
    }
    const std::string said = std::format("Jolt's assert failed at {}:{}: {} {}\n", file, line,
                                         expression, message != nullptr ? message : "");
    std::fputs(said.c_str(), stderr);
    return true;
}
#endif

// Jolt's allocator, its types, and the job system: started once, and never stopped, since a
// process ending stops its threads before a DLL could.
struct jolt_t {
    struct registered_t {
        registered_t() {
            JPH::RegisterDefaultAllocator();
            JPH::Trace = trace;
            JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = assert_failed;)
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }
    };

    registered_t registered;
    JPH::TempAllocatorImplWithMallocFallback memory{ step_memory_size };
    JPH::JobSystemThreadPool jobs{ max_jobs, max_barriers, thread_count() };
};

jolt_t* started = nullptr;

jolt_t& jolt() {
    if (started == nullptr) {
        started = new jolt_t();
    }
    return *started;
}

pool_t<world_t>& worlds() {
    static pool_t<world_t> pool;
    return pool;
}

bool finite(const fbe_physics_vec3_t& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

} // namespace

namespace fbe::physics {

world_t::world_t(std::uint32_t max_bodies)
    : broad_phase_layers(object_layer_count, broad_phase_layer_count),
      object_layers(object_layer_count) {
    broad_phase_layers.MapObjectToBroadPhaseLayer(not_moving_layer, not_moving_broad_phase);
    broad_phase_layers.MapObjectToBroadPhaseLayer(moving_layer, moving_broad_phase);
    object_layers.EnableCollision(not_moving_layer, moving_layer);
    object_layers.EnableCollision(moving_layer, moving_layer);
    // It reads the layers as it's made.
    object_vs_broad_phase = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(
        broad_phase_layers, broad_phase_layer_count, object_layers, object_layer_count);
    system.Init(max_bodies, 0, std::max(max_bodies, fewest_pairs),
                std::max(max_bodies / 2, fewest_pairs), broad_phase_layers, *object_vs_broad_phase,
                object_layers);
}

bool fail(std::string_view message) {
    last_error = message;
    return false;
}

world_t* find_world(std::uint64_t handle) {
    world_t* world = worlds().get(handle);
    if (world == nullptr) {
        fail(handle == 0 ? "there's no world" : "the world was freed");
    }
    return world;
}

JPH::JobSystem& jobs() {
    return jolt().jobs;
}

JPH::TempAllocator& step_memory() {
    return jolt().memory;
}

} // namespace fbe::physics

extern "C" {

FBE_PHYSICS_API const char* fbe_physics_version(void) {
    return FBE_VERSION;
}

FBE_PHYSICS_API const char* fbe_physics_error(void) {
    return last_error.c_str();
}

FBE_PHYSICS_API int32_t fbe_physics_threads(void) {
    return started != nullptr ? started->jobs.GetMaxConcurrency() - 1 : thread_count();
}

FBE_PHYSICS_API bool fbe_physics_set_threads(int32_t threads) {
    if (threads < 0 || threads > FBE_PHYSICS_MAX_THREADS) {
        return fail(
            std::format("Jolt has 0 to {} threads, not {}", FBE_PHYSICS_MAX_THREADS, threads));
    }
    threads_wanted = threads;
    if (started != nullptr && started->jobs.GetMaxConcurrency() - 1 != threads) {
        started->jobs.SetNumThreads(threads);
    }
    return true;
}

FBE_PHYSICS_API int32_t fbe_physics_worlds(void) {
    return static_cast<int32_t>(worlds().count());
}

FBE_PHYSICS_API uint64_t fbe_physics_new(uint32_t max_bodies) {
    if (max_bodies < 1 || max_bodies > FBE_PHYSICS_MAX_BODIES) {
        fail(std::format("a world is for 1 to {} bodies, not {}", FBE_PHYSICS_MAX_BODIES,
                         max_bodies));
        return 0;
    }
    jolt();
    return worlds().add(std::make_unique<world_t>(max_bodies));
}

FBE_PHYSICS_API bool fbe_physics_free(uint64_t world) {
    world_t* found = find_world(world);
    if (found == nullptr) {
        return false;
    }
    free_bodies(*found);
    return worlds().remove(world);
}

FBE_PHYSICS_API int32_t fbe_physics_bodies(uint64_t world) {
    const world_t* found = find_world(world);
    return found != nullptr ? static_cast<int32_t>(found->bodies.size()) : -1;
}

FBE_PHYSICS_API bool fbe_physics_optimize(uint64_t world) {
    world_t* found = find_world(world);
    if (found == nullptr) {
        return false;
    }
    found->system.OptimizeBroadPhase();
    return true;
}

FBE_PHYSICS_API uint32_t fbe_physics_max_bodies(uint64_t world) {
    const world_t* found = find_world(world);
    return found != nullptr ? found->system.GetMaxBodies() : 0;
}

FBE_PHYSICS_API bool fbe_physics_set_gravity(uint64_t world, const fbe_physics_vec3_t* gravity) {
    world_t* found = find_world(world);
    if (found == nullptr) {
        return false;
    }
    if (!finite(*gravity)) {
        return fail("gravity is a finite vector");
    }
    found->system.SetGravity(JPH::Vec3(gravity->x, gravity->y, gravity->z));
    return true;
}

FBE_PHYSICS_API bool fbe_physics_gravity(uint64_t world, fbe_physics_vec3_t* gravity) {
    const world_t* found = find_world(world);
    if (found == nullptr) {
        return false;
    }
    const JPH::Vec3 g = found->system.GetGravity();
    *gravity = { .x = g.GetX(), .y = g.GetY(), .z = g.GetZ() };
    return true;
}

FBE_PHYSICS_API bool fbe_physics_step(uint64_t world, float seconds, int32_t collision_steps) {
    world_t* found = find_world(world);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(seconds) || seconds <= 0.0F || seconds > 1.0F) {
        return fail(std::format("a step is more than 0 seconds and at most 1, not {}", seconds));
    }
    if (collision_steps < 1 || collision_steps > FBE_PHYSICS_MAX_COLLISION_STEPS) {
        return fail(std::format("a step finds collisions 1 to {} times, not {}",
                                FBE_PHYSICS_MAX_COLLISION_STEPS, collision_steps));
    }
    JPH::EPhysicsUpdateError errors = JPH::EPhysicsUpdateError::None;
    for (std::int32_t done = 0; done < collision_steps;) {
        const std::int32_t these = std::min(collision_steps - done, collision_steps_an_update);
        const float part =
            seconds * static_cast<float>(these) / static_cast<float>(collision_steps);
        errors |= found->system.Update(part, these, &step_memory(), &jobs());
        done += these;
    }
    found->overflowed = errors != JPH::EPhysicsUpdateError::None;
    ++found->steps;
    return true;
}

FBE_PHYSICS_API int64_t fbe_physics_steps(uint64_t world) {
    const world_t* found = find_world(world);
    return found != nullptr ? found->steps : -1;
}

FBE_PHYSICS_API bool fbe_physics_overflowed(uint64_t world) {
    const world_t* found = find_world(world);
    return found != nullptr && found->overflowed;
}

} // extern "C"
