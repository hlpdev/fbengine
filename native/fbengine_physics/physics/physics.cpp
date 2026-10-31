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
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace fbe::physics;

thread_local std::string last_error;

// What a step's jobs can be, as Jolt's samples have them.
constexpr std::uint32_t max_jobs = 2048;
constexpr std::uint32_t max_barriers = 64;
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

    jolt_t() {
        // Jolt's threads, as they start, take the mode its work runs in, which they keep.
        jobs.SetThreadInitFunction([](int) {
            thread_local std::optional<fp_mode_t> mode;
            mode.emplace();
        });
        jobs.Init(max_jobs, max_barriers, thread_count());
    }

    registered_t registered;
    JPH::TempAllocatorImplWithMallocFallback memory{ step_memory_size };
    JPH::JobSystemThreadPool jobs;
};

jolt_t* started = nullptr;

jolt_t& jolt() {
    if (started == nullptr) {
        started = new jolt_t();
    }
    return *started;
}

pool_t<world_t>& worlds() {
    // Never freed, as Jolt isn't: statics freed as the process ends go in any order, and a world
    // left then would free its bodies into a pool that had gone.
    static auto* pool = new pool_t<world_t>(WORLD_KIND);
    return *pool;
}

bool finite(const fbe_physics_vec3_t& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

} // namespace

namespace fbe::physics {

void start() {
    jolt();
}

void changed(world_t& world) {
    // Jolt's broad phase has about 1.3 nodes for each of its bodies, and keeps those that bodies
    // added and taken out use until it's next built, at a step or an optimize; half its bodies'
    // worth of them leaves room.
    if (++world.changes >= std::max(world.system.GetMaxBodies() / 2, 1U)) {
        world.system.OptimizeBroadPhase();
        world.changes = 0;
    }
}

bool normalized(float x, float y, float z, float w, JPH::Quat& made) {
    const double length =
        std::sqrt((double{ x } * x) + (double{ y } * y) + (double{ z } * z) + (double{ w } * w));
    if (!std::isfinite(length) || length == 0.0) {
        return fail("a rotation is finite, and not 0");
    }
    made = JPH::Quat(static_cast<float>(x / length), static_cast<float>(y / length),
                     static_cast<float>(z / length), static_cast<float>(w / length));
    return true;
}

world_t::world_t(std::uint32_t max_bodies) {
    system.Init(max_bodies, 0, std::max(max_bodies, fewest_pairs),
                std::max(max_bodies / 2, fewest_pairs), broad_phase, object_vs_broad_phase, layers);
    system.SetContactListener(&contact_listener);
}

// Its joints go before the bodies they join, and its characters, which own theirs, before the
// rest of its bodies.
world_t::~world_t() {
    free_joints(*this);
    free_characters(*this);
    free_bodies(*this);
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
    const world_t* found = find_world(world);
    if (found == nullptr) {
        return false;
    }
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
    found->changes = 0;
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
    const fp_mode_t mode;
    found->began.clear();
    found->ended.clear();
    // Each collision step is an update of Jolt's of its own, whose contacts are taken as it ends:
    // in one, a pair of parts begins or ends touching at most once, so sorting them puts them in
    // the order they happened, the same however Jolt's threads ran, and an update's jobs, all made
    // as it starts, always fit.
    const float part = seconds / static_cast<float>(collision_steps);
    JPH::EPhysicsUpdateError errors = JPH::EPhysicsUpdateError::None;
    for (std::int32_t i = 0; i < collision_steps; ++i) {
        before_contacts(*found);
        errors |= found->system.Update(part, 1, &step_memory(), &jobs());
        take_contacts(*found);
    }
    found->overflowed = errors != JPH::EPhysicsUpdateError::None;
    found->changes = 0;
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

FBE_PHYSICS_API bool fbe_physics_set_collides(uint64_t world, int32_t a, int32_t b, bool collides) {
    world_t* found = find_world(world);
    if (found == nullptr) {
        return false;
    }
    if (a < 0 || a >= FBE_PHYSICS_LAYERS || b < 0 || b >= FBE_PHYSICS_LAYERS) {
        return fail(std::format("a layer is 0 to {}, not {} and {}", FBE_PHYSICS_LAYERS - 1, a, b));
    }
    found->layers.set_collides(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b),
                               collides);
    return true;
}

FBE_PHYSICS_API bool fbe_physics_collides(uint64_t world, int32_t a, int32_t b) {
    const world_t* found = find_world(world);
    if (found == nullptr) {
        return false;
    }
    if (a < 0 || a >= FBE_PHYSICS_LAYERS || b < 0 || b >= FBE_PHYSICS_LAYERS) {
        return fail(std::format("a layer is 0 to {}, not {} and {}", FBE_PHYSICS_LAYERS - 1, a, b));
    }
    return found->layers.collides(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b));
}

} // extern "C"
