// A world's state, saved and restored: Jolt's own, its steps, what touches what, and its
// characters', so a world restored steps on exactly as it did from where it was saved, as
// rewinding a game played over a network, or a replay, needs. It's of the bodies, joints, and
// characters the world had, which a world restored into has to have too.

#include "fbengine_physics.h"
#include "physics/physics.hpp"

#include <algorithm>
#include <cstdint>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <string>
#include <utility>
#include <vector>

namespace fbe::physics {

namespace {

// "fbps", and the version of what follows it.
constexpr std::uint32_t magic = 0x73706266;
constexpr std::uint32_t version = 1;

std::vector<std::uint64_t> sorted(std::vector<std::uint64_t> handles) {
    std::ranges::sort(handles);
    return handles;
}

void write_handles(JPH::StateRecorder& state, const std::vector<std::uint64_t>& handles) {
    state.Write(static_cast<std::uint64_t>(handles.size()));
    for (const std::uint64_t handle : sorted(handles)) {
        state.Write(handle);
    }
}

// Whether `state`'s handles are `handles`.
bool same_handles(JPH::StateRecorder& state, const std::vector<std::uint64_t>& handles) {
    std::uint64_t count = 0;
    state.Read(count);
    if (failed(state) || count != handles.size()) {
        return false;
    }
    for (const std::uint64_t handle : sorted(handles)) {
        std::uint64_t saved = 0;
        state.Read(saved);
        if (saved != handle) {
            return false;
        }
    }
    return !failed(state);
}

void write_touching(const world_t& world, JPH::StateRecorder& state) {
    state.Write(static_cast<std::uint64_t>(world.touching.size()));
    for (const auto& [key, pair] : world.touching) {
        state.Write(key);
        state.Write(pair.a);
        state.Write(pair.b);
        state.Write(pair.a_id.GetIndexAndSequenceNumber());
        state.Write(pair.b_id.GetIndexAndSequenceNumber());
        state.Write(static_cast<std::uint64_t>(pair.parts.size()));
        for (const auto& [part, kept] : pair.parts) {
            state.Write(part);
            state.Write(kept);
        }
    }
}

bool read_touching(world_t& world, JPH::StateRecorder& state) {
    std::uint64_t count = 0;
    state.Read(count);
    world.touching.clear();
    for (std::uint64_t i = 0; i < count && !failed(state); ++i) {
        std::uint64_t key = 0;
        touching_t pair;
        JPH::uint32 a_id = 0;
        JPH::uint32 b_id = 0;
        std::uint64_t parts = 0;
        state.Read(key);
        state.Read(pair.a);
        state.Read(pair.b);
        state.Read(a_id);
        state.Read(b_id);
        state.Read(parts);
        pair.a_id = JPH::BodyID(a_id);
        pair.b_id = JPH::BodyID(b_id);
        for (std::uint64_t j = 0; j < parts && !failed(state); ++j) {
            std::uint64_t part = 0;
            bool kept = false;
            state.Read(part);
            state.Read(kept);
            pair.parts[part] = kept;
        }
        world.touching[key] = pair;
    }
    return !failed(state);
}

// Saves all of `world`'s state, with `carried`, into `state`.
void save(const world_t& world, double carried, JPH::StateRecorder& state) {
    state.Write(magic);
    state.Write(version);
    write_handles(state, world.bodies);
    write_handles(state, world.joints);
    write_handles(state, world.characters);
    world.system.SaveState(state);
    state.Write(world.steps);
    state.Write(world.overflowed);
    state.Write(carried);
    write_touching(world, state);
    save_characters(world, state);
}

// Restores `world` from `state`, putting what it carried in `carried`; false, saying why, if it
// isn't a state of this world's bodies, joints, and characters, or it ends too soon.
bool restore(world_t& world, JPH::StateRecorder& state, double& carried) {
    std::uint32_t saved_magic = 0;
    std::uint32_t saved_version = 0;
    state.Read(saved_magic);
    state.Read(saved_version);
    if (failed(state) || saved_magic != magic || saved_version != version) {
        return fail("that isn't a world's state");
    }
    if (!same_handles(state, world.bodies) || !same_handles(state, world.joints) ||
        !same_handles(state, world.characters)) {
        return fail("the state is of other bodies, joints, or characters than the world has");
    }
    if (!world.system.RestoreState(state)) {
        return fail("the state ends too soon");
    }
    state.Read(world.steps);
    state.Read(world.overflowed);
    state.Read(carried);
    if (failed(state) || !read_touching(world, state) || !restore_characters(world, state)) {
        return fail("the state ends too soon");
    }
    world.changes = 0;
    return true;
}

} // namespace

} // namespace fbe::physics

extern "C" {

FBE_PHYSICS_API int64_t fbe_physics_save(uint64_t world, double carried, uint8_t* bytes,
                                         int64_t capacity) {
    using namespace fbe::physics;
    const world_t* found = find_world(world);
    if (found == nullptr) {
        return -1;
    }
    JPH::StateRecorderImpl state;
    save(*found, carried, state);
    const std::string data = state.GetData();
    if (std::cmp_greater_equal(capacity, data.size())) {
        std::ranges::copy(data, bytes);
    }
    return static_cast<int64_t>(data.size());
}

FBE_PHYSICS_API bool fbe_physics_restore(uint64_t world, const uint8_t* bytes, int64_t size,
                                         double* carried) {
    using namespace fbe::physics;
    world_t* found = find_world(world);
    if (found == nullptr) {
        return false;
    }
    if (size <= 0) {
        return fail("that isn't a world's state");
    }
    // What the world is now, put back if the state turns out not to be whole.
    JPH::StateRecorderImpl now;
    save(*found, 0.0, now);
    JPH::StateRecorderImpl state;
    state.WriteBytes(bytes, static_cast<std::size_t>(size));
    state.Rewind();
    if (!restore(*found, state, *carried)) {
        now.Rewind();
        double ignored = 0.0;
        // NOLINTNEXTLINE(bugprone-unused-return-value): the world's own state restores.
        static_cast<void>(restore(*found, now, ignored));
        return false;
    }
    return true;
}

} // extern "C"
