// A world's state, saved and restored: Jolt's own, its steps, what touches what, and its
// characters', so a world restored steps on exactly as it did from where it was saved, as
// rewinding a game played over a network, or a replay, needs. It's of the bodies, joints, and
// characters the world had, which a world restored into has to have too.
//
// A state is a header, then what it holds: the header says what saved it, fbengine_physics' and
// Jolt's versions, since what follows is mostly Jolt's own, and how long what follows is, with a
// hash of it, so a state changed or cut short is refused before any of it's put back.

#include "fbengine_physics.h"
#include "physics/physics.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <Jolt/Core/Core.h>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace fbe::physics {

namespace {

// "fbps", and the version of what follows it.
constexpr std::uint32_t magic = 0x73706266;
constexpr std::uint32_t format = 2;
constexpr std::uint32_t jolt_version = (std::uint32_t{ JPH_VERSION_MAJOR } << 16U) |
                                       (std::uint32_t{ JPH_VERSION_MINOR } << 8U) |
                                       std::uint32_t{ JPH_VERSION_PATCH };
// fbengine's version, as "0.1.1", padded with 0s.
constexpr std::size_t version_size = 16;
static_assert(std::string_view(FBE_VERSION).size() < version_size);

// The header's magic, format, Jolt's version, fbengine's, and the length and hash of what
// follows.
constexpr std::size_t header_size = 4 + 4 + 4 + version_size + 8 + 8;

// FNV-1a, 64 bits.
std::uint64_t hash_of(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    for (const std::uint8_t byte : bytes) {
        hash = (hash ^ byte) * 0x100000001B3ULL;
    }
    return hash;
}

// A state written into memory, or read from memory, as Jolt's StateRecorderImpl does through a
// string stream, without its copies.
class recorder_t final : public JPH::StateRecorder {
public:
    // One to write into `written`, after what it has.
    explicit recorder_t(std::vector<std::uint8_t>& written) : m_written(&written) {}

    // One to read `bytes`.
    explicit recorder_t(std::span<const std::uint8_t> bytes) : m_read(bytes) {}

    void WriteBytes(const void* data, std::size_t size) override {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        m_written->insert(m_written->end(), bytes, bytes + size);
    }

    void ReadBytes(void* data, std::size_t size) override {
        if (m_failed || size > m_read.size() - m_at) {
            m_failed = true;
            std::memset(data, 0, size);
            return;
        }
        std::memcpy(data, m_read.data() + m_at, size);
        m_at += size;
    }

    [[nodiscard]] bool IsEOF() const override {
        return m_at == m_read.size();
    }

    [[nodiscard]] bool IsFailed() const override {
        return m_failed;
    }

private:
    std::vector<std::uint8_t>* m_written = nullptr;
    std::span<const std::uint8_t> m_read;
    std::size_t m_at = 0;
    bool m_failed = false;
};

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

// Writes `value`'s bytes at `at`, moving it on.
template <typename T>
void put(std::uint8_t*& at, const T& value) {
    std::memcpy(at, &value, sizeof(value));
    at += sizeof(value);
}

template <typename T>
T take(const std::uint8_t*& at) {
    T value{};
    std::memcpy(&value, at, sizeof(value));
    at += sizeof(value);
    return value;
}

// Saves all of `world`'s state, with `carried`, into world.saved: a header, then what it holds.
void save(world_t& world, double carried) {
    std::vector<std::uint8_t>& saved = world.saved;
    saved.assign(header_size, 0);
    recorder_t state(saved);
    write_handles(state, world.bodies);
    write_handles(state, world.joints);
    write_handles(state, world.characters);
    world.system.SaveState(state);
    state.Write(world.steps);
    state.Write(world.overflowed);
    state.Write(carried);
    write_touching(world, state);
    save_characters(world, state);
    std::array<char, version_size> version{};
    std::ranges::copy(std::string_view(FBE_VERSION), version.begin());
    const std::span held = std::span(saved).subspan(header_size);
    std::uint8_t* at = saved.data();
    put(at, magic);
    put(at, format);
    put(at, jolt_version);
    put(at, version);
    put(at, static_cast<std::uint64_t>(held.size()));
    put(at, hash_of(held));
}

// What `bytes` hold after their header, if it says fbengine_physics as it is saved them and
// they're as they were saved, or an empty span, saying why.
std::span<const std::uint8_t> held_by(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < header_size) {
        fail("that isn't a world's state");
        return {};
    }
    const std::uint8_t* at = bytes.data();
    if (take<std::uint32_t>(at) != magic) {
        fail("that isn't a world's state");
        return {};
    }
    const auto saved_format = take<std::uint32_t>(at);
    const auto saved_jolt = take<std::uint32_t>(at);
    const auto saved_version = take<std::array<char, version_size>>(at);
    std::array<char, version_size> ours{};
    std::ranges::copy(std::string_view(FBE_VERSION), ours.begin());
    if (saved_format != format || saved_jolt != jolt_version || saved_version != ours) {
        fail("the state was saved by another version of fbengine_physics");
        return {};
    }
    const auto length = take<std::uint64_t>(at);
    const auto hash = take<std::uint64_t>(at);
    const std::span held = bytes.subspan(header_size);
    if (length != held.size() || hash != hash_of(held)) {
        fail("that isn't a world's state");
        return {};
    }
    return held;
}

// Restores `world` from what a state holds, putting what it carried in `carried`; false, saying
// why, if it isn't of this world's bodies, joints, and characters, which is known before any of
// it's put back.
bool restore(world_t& world, std::span<const std::uint8_t> held, double& carried) {
    recorder_t state(held);
    if (!same_handles(state, world.bodies) || !same_handles(state, world.joints) ||
        !same_handles(state, world.characters)) {
        return fail("the state is of other bodies, joints, or characters than the world has");
    }
    // A state whose hash is right is whole, as it was saved, so what follows reads.
    if (!world.system.RestoreState(state)) {
        return fail("the state ends too soon");
    }
    state.Read(world.steps);
    state.Read(world.overflowed);
    state.Read(carried);
    if (failed(state) || !read_touching(world, state) || !restore_characters(world, state)) {
        return fail("the state ends too soon");
    }
    // What began and ended was in the step before, which the state's isn't.
    world.began.clear();
    world.ended.clear();
    world.changes = 0;
    return true;
}

} // namespace

} // namespace fbe::physics

extern "C" {

FBE_PHYSICS_API int64_t fbe_physics_save(uint64_t world, double carried) {
    using namespace fbe::physics;
    const fp_mode_t fp_mode;
    world_t* found = find_world(world);
    if (found == nullptr) {
        return -1;
    }
    save(*found, carried);
    return static_cast<int64_t>(found->saved.size());
}

FBE_PHYSICS_API int64_t fbe_physics_saved(uint64_t world, uint8_t* bytes, int64_t capacity) {
    using namespace fbe::physics;
    const world_t* found = find_world(world);
    if (found == nullptr) {
        return -1;
    }
    if (std::cmp_greater_equal(capacity, found->saved.size())) {
        std::ranges::copy(found->saved, bytes);
    }
    return static_cast<int64_t>(found->saved.size());
}

FBE_PHYSICS_API bool fbe_physics_restore(uint64_t world, const uint8_t* bytes, int64_t size,
                                         double* carried) {
    using namespace fbe::physics;
    const fp_mode_t fp_mode;
    world_t* found = find_world(world);
    if (found == nullptr) {
        return false;
    }
    if (size <= 0) {
        return fail("that isn't a world's state");
    }
    const std::span held = held_by(std::span(bytes, static_cast<std::size_t>(size)));
    if (held.empty() || !restore(*found, held, *carried)) {
        return false;
    }
    ++found->moves;
    return true;
}

} // extern "C"
