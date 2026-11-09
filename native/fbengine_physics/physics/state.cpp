// A world's state, saved and restored: Jolt's own, its steps, what touches what, and its
// characters', so a world restored steps on exactly as it did from where it was saved, as
// rewinding a game played over a network, or a replay, needs. It's of the bodies, joints, and
// characters the world had, which a world restored into has to have too.
//
// A state is a header, then what it holds: the header says what saved it, fbengine_physics' and
// Jolt's versions, since what follows is mostly Jolt's own, and how long what follows is, with a
// hash of it, so a state changed or cut short is refused before any of it's put back.
//
// A hash keeps out what changed by chance, but a state made to pass it, as one sent over a network
// could be, holds whatever its maker liked, and Jolt puts its own back as it finds it. So all of a
// state is read first as restoring it would, to know it's one a world could be in: of this world's
// bodies, joints, and characters as they are, in the order they're saved in, with finite numbers,
// rotations of length 1, flags of 0 or 1, and no more or less than it says.

#include "fbengine_physics.h"
#include "physics/physics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <Jolt/Core/Core.h>
#include <Jolt/Geometry/Sphere.h>
#include <Jolt/Math/Vector.h>
#include <Jolt/Physics/Body/BodyPair.h>
#include <Jolt/Physics/Body/MotionType.h>
#include <Jolt/Physics/Character/CharacterBase.h>
#include <Jolt/Physics/Character/CharacterID.h>
#include <Jolt/Physics/Collision/Shape/SubShapeIDPair.h>
#include <Jolt/Physics/Constraints/ContactConstraintManager.h>
#include <Jolt/Physics/Constraints/MotorSettings.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <limits>
#include <map>
#include <optional>
#include <set>
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
// fbengine's version, as "1.0.0-rc.2", padded with 0s.
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

// What write_touching does, but for the handles, which are of the whole program, not of the world.
void write_touching_of_world(const world_t& world, JPH::StateRecorder& state) {
    state.Write(static_cast<std::uint64_t>(world.touching.size()));
    for (const auto& [key, pair] : world.touching) {
        state.Write(key);
        state.Write(pair.a_id.GetIndexAndSequenceNumber());
        state.Write(pair.b_id.GetIndexAndSequenceNumber());
        state.Write(static_cast<std::uint64_t>(pair.parts.size()));
        for (const auto& [part, kept] : pair.parts) {
            state.Write(part);
            state.Write(kept);
        }
    }
}

// Reads a state as Jolt's and this file's restores do, the same types in the same order, but into
// what it throws away, to know it's sound before any of it's put back. It's of Jolt as the state's
// header says, which only a state of that Jolt gets past.
class checker_t {
public:
    checker_t(const world_t& world, std::span<const std::uint8_t> held)
        : m_world(&world), m_state(held) {}

    // Whether the state's all sound, and all read.
    bool sound() {
        const world_t& world = *m_world;
        if (!same_handles(m_state, world.bodies) || !same_handles(m_state, world.joints) ||
            !same_handles(m_state, world.characters)) {
            return false;
        }
        jolt();
        ours();
        return m_sound && !failed(m_state) && m_state.IsEOF();
    }

private:
    template <typename T>
    T read() {
        T value{};
        m_state.Read(value);
        return value;
    }

    void need(bool sound) {
        m_sound = m_sound && sound;
    }

    // Whether the state's still being read: sound, and not run out.
    [[nodiscard]] bool going() const {
        return m_sound && !failed(m_state);
    }

    // Numbers each at most `most` from 0, as no NaN is.
    float number(float most = std::numeric_limits<float>::max()) {
        const auto x = read<float>();
        need(std::abs(x) <= most);
        return x;
    }

    void vec3(float most = std::numeric_limits<float>::max()) {
        need(within(read<JPH::Vec3>(), most));
    }

    // A Float3, as a Vec3.
    JPH::Vec3 kept_float3(float most = std::numeric_limits<float>::max()) {
        const auto v = read<JPH::Float3>();
        need(within(v.x, v.y, v.z, most));
        return { v.x, v.y, v.z };
    }

    void float3(float most = std::numeric_limits<float>::max()) {
        static_cast<void>(kept_float3(most));
    }

    void vec2(float most = std::numeric_limits<float>::max()) {
        const auto v = read<JPH::Vector<2>>();
        need(std::abs(v[0]) <= most && std::abs(v[1]) <= most);
    }

    // A direction, of length 1, as Jolt keeps a contact's normal.
    void direction() {
        need(kept_float3().IsNormalized(1.0e-5F));
    }

    JPH::Quat rotation() {
        const auto q = read<JPH::Quat>();
        const bool turns = !q.IsNaN() && q.IsNormalized();
        need(turns);
        return turns ? q : JPH::Quat::sIdentity();
    }

    // The most a constraint's or a contact's impulse, warming a step's start, goes, so the bodies
    // it pushes, by `response`, change their velocities by no more than most_motion.
    static float most_impulse(float response) {
        return response > 0.0F ? most_motion / response : most_motion;
    }

    // A bool, as one byte, 0 or 1, which reading into a bool would take for granted.
    bool flag() {
        static_assert(sizeof(bool) == sizeof(std::uint8_t));
        const auto value = read<std::uint8_t>();
        need(value <= 1);
        return value == 1;
    }

    // One of an enum's values, `last` the last of them.
    template <typename E>
    void choice(E last) {
        const auto value = static_cast<std::uint64_t>(read<E>());
        need(value <= static_cast<std::uint64_t>(last));
    }

    // PhysicsSystem::RestoreState's.
    void jolt() {
        need(read<JPH::EStateRecorderState>() == JPH::EStateRecorderState::All);
        // The last step's time, at most a step's, and gravity.
        need(number(1.0F) >= 0.0F);
        vec3(most_gravity);
        bodies();
        contacts();
        constraints();
    }

    // BodyManager::RestoreState's, of the bodies in its broad phase, in its order, which is the
    // order it saves them in.
    void bodies() {
        JPH::BodyIDVector ids;
        m_world->system.GetBodies(ids);
        const JPH::BodyLockInterfaceNoLock& locks = m_world->system.GetBodyLockInterfaceNoLock();
        std::vector<const JPH::Body*> saved;
        for (const JPH::BodyID id : ids) {
            const JPH::Body* body = locks.TryGetBody(id);
            if (body != nullptr && body->IsInBroadPhase()) {
                saved.push_back(body);
            }
        }
        need(read<JPH::uint32>() == saved.size());
        for (const JPH::Body* body : saved) {
            if (!going()) {
                return;
            }
            need(read<JPH::BodyID>() == body->GetID());
            // What doesn't move has nowhere to keep being awake.
            need(!flag() || !body->IsStatic());
            vec3(most_position);
            const JPH::Quat turned = rotation();
            const JPH::MotionProperties* motion = body->GetMotionPropertiesUnchecked();
            if (motion != nullptr) {
                need(!body->IsSoftBody());
                // MotionProperties::RestoreState's: its velocities, and its forces, and what
                // they'd change its velocities by in a second, by its mass and inertia.
                vec3(most_motion);
                vec3(most_motion);
                const JPH::Vec3 force = kept_float3();
                const JPH::Vec3 torque = kept_float3();
                if (body->IsDynamic()) {
                    const JPH::Mat44 turning =
                        motion->GetInverseInertiaForRotation(JPH::Mat44::sRotation(turned));
                    need(within(force * motion->GetInverseMass(), most_motion) &&
                         within(turning * torque, most_motion));
                } else {
                    need(within(force, most_motion) && within(torque, most_motion));
                }
                for (const JPH::Sphere& sphere : read<std::array<JPH::Sphere, 3>>()) {
                    need(within(sphere.GetCenter(), most_position) &&
                         std::abs(sphere.GetRadius()) <= most_position);
                }
                number();
                flag();
            }
        }
    }

    // ContactConstraintManager::ManifoldCache::RestoreState's: the pairs of bodies that touched,
    // and their manifolds, then the manifolds of continuous collisions, each in the order Jolt
    // saves them in, by their keys, which are all of one map, so none is twice.
    void contacts() {
        std::set<JPH::SubShapeIDPair> keys;
        const JPH::BodyLockInterfaceNoLock& locks = m_world->system.GetBodyLockInterfaceNoLock();
        // No more pairs, or manifolds, than Jolt has room for, which it'd run out of partway.
        const auto pairs = read<JPH::uint32>();
        need(pairs <= m_world->most_pairs);
        std::optional<JPH::BodyPair> last_pair;
        for (JPH::uint32 i = 0; i < pairs && going(); ++i) {
            const auto pair = read<JPH::BodyPair>();
            need(!last_pair.has_value() || *last_pair < pair);
            last_pair = pair;
            const JPH::Body* a = locks.TryGetBody(pair.mBodyA);
            const JPH::Body* b = locks.TryGetBody(pair.mBodyB);
            const float most = most_impulse(std::max(response(a), response(b)));
            // Where its points are on each body, near it.
            const float near = std::max(reach(a), reach(b));
            // How far they moved and turned since.
            float3();
            float3();
            const auto manifolds = read<JPH::uint32>();
            std::optional<JPH::SubShapeIDPair> last_manifold;
            for (JPH::uint32 j = 0; j < manifolds && going(); ++j) {
                const auto manifold = read<JPH::SubShapeIDPair>();
                need(manifold.GetBody1ID() == pair.mBodyA && manifold.GetBody2ID() == pair.mBodyB);
                need(!last_manifold.has_value() || *last_manifold < manifold);
                last_manifold = manifold;
                keys.insert(manifold);
                const auto points = read<JPH::uint16>();
                need(points <= JPH::ContactConstraintManager::MaxContactPoints);
                // Its normal, and its friction's impulses.
                direction();
                vec2(most);
                number(most);
                for (JPH::uint16 k = 0; k < points && going(); ++k) {
                    float3(near);
                    float3(near);
                    number(most);
                }
            }
        }
        const auto continuous = read<JPH::uint32>();
        need(keys.size() + continuous <= m_world->most_manifolds);
        std::optional<JPH::SubShapeIDPair> last;
        for (JPH::uint32 i = 0; i < continuous && going(); ++i) {
            const auto manifold = read<JPH::SubShapeIDPair>();
            need((!last.has_value() || *last < manifold) && !keys.contains(manifold));
            last = manifold;
        }
    }

    // ConstraintManager::RestoreState's, each constraint's own by its kind, of fbengine_physics'
    // joints'.
    void constraints() {
        const JPH::Constraints all = m_world->system.GetConstraints();
        need(read<JPH::uint32>() == all.size());
        // Each by where it is in Jolt's list, which it saves as its index.
        for (JPH::uint32 index = 0; index < all.size() && going(); ++index) {
            const JPH::Constraint& constraint = *all[index];
            need(read<JPH::uint32>() == index);
            // Every joint's is between two bodies.
            const auto& joined = static_cast<const JPH::TwoBodyConstraint&>(constraint);
            const float most =
                most_impulse(std::max(response(joined.GetBody1()), response(joined.GetBody2())));
            // Whether it's on.
            flag();
            switch (constraint.GetSubType()) {
            case JPH::EConstraintSubType::Fixed:
                vec3(most);
                vec3(most);
                break;
            case JPH::EConstraintSubType::Point:
                vec3(most);
                break;
            case JPH::EConstraintSubType::Hinge:
            case JPH::EConstraintSubType::Slider:
                // Its motor's, how it's held turning or sliding only one way, where, its limits',
                // then its motor's state, none fbe_joint_set_motor doesn't set, and its targets: a
                // speed, and a hinge's angle, -pi to pi, which Jolt brings one to a turn at a time,
                // so one far off hung a step, or a slider's position.
                number(most);
                vec2(most);
                vec3(most);
                number(most);
                choice(JPH::EMotorState::Position);
                number(most_motion);
                number(constraint.GetSubType() == JPH::EConstraintSubType::Hinge ? JPH::JPH_PI
                                                                                 : most_position);
                break;
            case JPH::EConstraintSubType::Distance:
                number(most);
                vec3(1.0F);
                break;
            default:
                need(false);
            }
        }
    }

    // What this file's restore and restore_characters read after Jolt's.
    void ours() {
        need(read<std::int64_t>() >= 0);
        flag();
        const auto carried = read<double>();
        need(std::isfinite(carried) && carried >= 0.0);
        touching();
        characters();
    }

    // The bodies touching, each pair by its key, in order, of the world's bodies, each of a part or
    // more.
    void touching() {
        std::map<std::uint64_t, JPH::uint32> ids;
        for (const std::uint64_t handle : m_world->bodies) {
            world_t* of = nullptr;
            const JPH::Body* body = find_body(handle, of);
            ids[handle] = body != nullptr ? body->GetID().GetIndexAndSequenceNumber()
                                          : JPH::BodyID::cInvalidBodyID;
        }
        const auto id_of = [&ids](std::uint64_t handle) {
            const auto found = ids.find(handle);
            return found != ids.end() ? found->second : JPH::BodyID::cInvalidBodyID;
        };
        const auto pairs = read<std::uint64_t>();
        std::optional<std::uint64_t> last_key;
        for (std::uint64_t i = 0; i < pairs && going(); ++i) {
            const auto key = read<std::uint64_t>();
            need(!last_key.has_value() || *last_key < key);
            last_key = key;
            const auto a = read<std::uint64_t>();
            const auto b = read<std::uint64_t>();
            const auto a_id = read<JPH::uint32>();
            const auto b_id = read<JPH::uint32>();
            need(a_id != JPH::BodyID::cInvalidBodyID && a_id == id_of(a) && b_id == id_of(b) &&
                 a_id < b_id && key == ((std::uint64_t{ a_id } << 32U) | b_id));
            const auto parts = read<std::uint64_t>();
            need(parts > 0);
            std::optional<std::uint64_t> last_part;
            for (std::uint64_t j = 0; j < parts && going(); ++j) {
                const auto part = read<std::uint64_t>();
                need(!last_part.has_value() || *last_part < part);
                last_part = part;
                flag();
            }
        }
    }

    // CharacterVirtual::RestoreState's, of each character in its handle's order.
    void characters() {
        for (const std::uint64_t handle : sorted(m_world->characters)) {
            if (!going()) {
                return;
            }
            need(read<std::uint64_t>() == handle);
            // CharacterBase's: what it stands on, its body and part, and where, its normal, and
            // how fast it goes.
            choice(JPH::CharacterBase::EGroundState::InAir);
            read<JPH::BodyID>();
            read<JPH::SubShapeID>();
            vec3(most_position);
            vec3(1.0F);
            vec3(most_motion);
            // Where it is, how it's turned, how fast it goes, its last move's time, and whether
            // it hit too much.
            vec3(most_position);
            static_cast<void>(rotation());
            vec3(most_motion);
            need(number(1.0F) >= 0.0F);
            flag();
            const auto contacts = read<JPH::uint32>();
            for (JPH::uint32 i = 0; i < contacts && going(); ++i) {
                // Its key: the body or character, and the part, and then where, how fast it goes,
                // its normal and its surface's, how far, how far along, what the other is, and
                // its flags.
                read<JPH::BodyID>();
                read<JPH::CharacterID>();
                read<JPH::SubShapeID>();
                vec3(most_position);
                vec3(most_motion);
                vec3(1.0F);
                vec3(1.0F);
                number(most_position);
                number(most_position);
                choice(JPH::EMotionType::Dynamic);
                flag();
                flag();
                flag();
                flag();
            }
        }
    }

    // How far from its center of mass a point of `body`'s shape is, at most, with room: what a
    // contact's points on it are within.
    static float reach(const JPH::Body* body) {
        if (body == nullptr) {
            return most_length;
        }
        const JPH::AABox bounds = body->GetShape()->GetLocalBounds();
        return 1.0F +
               (2.0F * std::max(bounds.mMin.Abs().ReduceMax(), bounds.mMax.Abs().ReduceMax()));
    }

    const world_t* m_world;
    recorder_t m_state;
    bool m_sound = true;
};

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
    // A header with nothing after it is no state either, though its hash is of nothing.
    if (held.empty() || length != held.size() || hash != hash_of(held)) {
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
    if (!checker_t(world, held).sound()) {
        return fail("that isn't a world's state");
    }
    // A state the checker's read through restores whole, with room in Jolt for all of it.
    if (!world.system.RestoreState(state)) {
        return fail("that isn't a world's state");
    }
    // What's read now is sound, as the checker read it.
    state.Read(world.steps);
    state.Read(world.overflowed);
    state.Read(carried);
    read_touching(world, state);
    restore_characters(world, state);
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

FBE_PHYSICS_API bool fbe_physics_state_hash(uint64_t world, double carried, uint64_t* hash) {
    using namespace fbe::physics;
    const fp_mode_t fp_mode;
    const world_t* found = find_world(world);
    if (found == nullptr) {
        return false;
    }
    std::vector<std::uint8_t> held;
    recorder_t state(held);
    found->system.SaveState(state);
    state.Write(found->steps);
    state.Write(found->overflowed);
    state.Write(carried);
    write_touching_of_world(*found, state);
    hash_characters(*found, state);
    *hash = hash_of(held);
    return true;
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
