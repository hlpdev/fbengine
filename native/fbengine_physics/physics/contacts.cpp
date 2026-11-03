// Contacts: Jolt says, on its threads, as it steps, which parts of which bodies began and stopped
// touching; once it's stepped, they're sorted, so they come out the same however its threads ran,
// and counted by pairs of bodies, which begin when their first parts touch and end when their last
// stop.

#include "fbengine_physics.h"
#include "physics/physics.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>

namespace fbe::physics {

namespace {

std::uint64_t pair_key(const JPH::BodyID& a, const JPH::BodyID& b) {
    return (std::uint64_t{ a.GetIndexAndSequenceNumber() } << 32U) | b.GetIndexAndSequenceNumber();
}

JPH::Vec3 velocity_at(const JPH::Body& body, JPH::RVec3Arg point) {
    return body.IsStatic() ? JPH::Vec3::sZero() : body.GetPointVelocity(point);
}

fbe_physics_vec3_t vec3(JPH::Vec3Arg v) {
    return { .x = v.GetX(), .y = v.GetY(), .z = v.GetZ() };
}

} // namespace

void contacts_t::OnContactAdded(const JPH::Body& a, const JPH::Body& b,
                                const JPH::ContactManifold& manifold,
                                [[maybe_unused]] JPH::ContactSettings& settings) {
    const JPH::RVec3 point = manifold.mRelativeContactPointsOn1.empty()
                                 ? manifold.mBaseOffset
                                 : manifold.GetWorldSpaceContactPointOn1(0);
    // The normal's from a to b, so they close as a's velocity along it is more than b's.
    const JPH::Vec3 relative = velocity_at(a, point) - velocity_at(b, point);
    const float closing = relative.Dot(manifold.mWorldSpaceNormal);
    contact_event_t event{
        .added = true,
        .a = a.GetID(),
        .b = b.GetID(),
        .a_part = manifold.mSubShapeID1,
        .b_part = manifold.mSubShapeID2,
        .a_handle = a.GetUserData(),
        .b_handle = b.GetUserData(),
        .position = JPH::Vec3(point),
        .normal = manifold.mWorldSpaceNormal,
        .speed = std::max(closing, 0.0F),
    };
    if (b.GetID() < a.GetID()) {
        std::swap(event.a, event.b);
        std::swap(event.a_part, event.b_part);
        std::swap(event.a_handle, event.b_handle);
        event.normal = -event.normal;
    }
    const std::scoped_lock lock(m_lock);
    m_events.push_back(event);
}

void contacts_t::OnContactRemoved(const JPH::SubShapeIDPair& pair) {
    contact_event_t event{
        .added = false,
        .a = pair.GetBody1ID(),
        .b = pair.GetBody2ID(),
        .a_part = pair.GetSubShapeID1(),
        .b_part = pair.GetSubShapeID2(),
        .a_handle = 0,
        .b_handle = 0,
        .position = JPH::Vec3::sZero(),
        .normal = JPH::Vec3::sZero(),
        .speed = 0.0F,
    };
    if (event.b < event.a) {
        std::swap(event.a, event.b);
        std::swap(event.a_part, event.b_part);
    }
    const std::scoped_lock lock(m_lock);
    m_events.push_back(event);
}

std::vector<contact_event_t> contacts_t::take() {
    const std::scoped_lock lock(m_lock);
    return std::exchange(m_events, {});
}

void before_contacts(world_t& world) {
    const JPH::BodyLockInterfaceNoLock& bodies = world.system.GetBodyLockInterfaceNoLock();
    const auto active = [&bodies](JPH::BodyID id) {
        const JPH::Body* body = bodies.TryGetBody(id);
        return body != nullptr && body->IsActive();
    };
    for (auto& [key, pair] : world.touching) {
        const bool kept =
            std::ranges::any_of(pair.parts, [](const auto& part) { return part.second; });
        pair.awake = kept && (active(pair.a_id) || active(pair.b_id));
    }
}

void take_contacts(world_t& world) {
    std::vector<contact_event_t> events = world.contact_listener.take();
    const auto order = [](const contact_event_t& e) {
        return std::tuple(pair_key(e.a, e.b), e.a_part.GetValue(), e.b_part.GetValue(), e.added);
    };
    std::ranges::sort(events, [&](const contact_event_t& x, const contact_event_t& y) {
        return order(x) < order(y);
    });
    const JPH::BodyLockInterfaceNoLock& bodies = world.system.GetBodyLockInterfaceNoLock();
    const auto active = [&bodies](JPH::BodyID id) {
        const JPH::Body* body = bodies.TryGetBody(id);
        return body != nullptr && body->IsActive();
    };
    // Whether each pair a step changed touched before it, and the first part of it found.
    std::map<std::uint64_t, bool> touched;
    std::map<std::uint64_t, const contact_event_t*> found;
    for (const contact_event_t& event : events) {
        const std::uint64_t key = pair_key(event.a, event.b);
        auto [entry, made] = world.touching.try_emplace(key);
        touching_t& pair = entry->second;
        if (made) {
            pair.a_id = event.a;
            pair.b_id = event.b;
        }
        touched.try_emplace(key, !pair.parts.empty());
        const std::uint64_t part =
            (std::uint64_t{ event.a_part.GetValue() } << 32U) | event.b_part.GetValue();
        if (event.added) {
            pair.parts[part] = false;
            pair.a = event.a_handle;
            pair.b = event.b_handle;
            found.try_emplace(key, &event);
        } else if (!active(event.a) && !active(event.b)) {
            // Jolt forgets what sleeping bodies touch, which they still do.
            if (auto kept = pair.parts.find(part); kept != pair.parts.end()) {
                kept->second = true;
            }
        } else {
            pair.parts.erase(part);
        }
    }
    // What a pair awake as the step began kept while it slept, and didn't find again, is gone.
    for (auto& [key, pair] : world.touching) {
        if (pair.awake &&
            std::erase_if(pair.parts, [](const auto& part) { return part.second; }) > 0) {
            touched.try_emplace(key, true);
        }
        pair.awake = false;
    }
    for (const auto& [key, before] : touched) {
        const auto entry = world.touching.find(key);
        const bool now = entry != world.touching.end() && !entry->second.parts.empty();
        if (!before && now) {
            const contact_event_t& first = *found.at(key);
            world.began.push_back({
                .a = first.a_handle,
                .b = first.b_handle,
                .position = vec3(first.position),
                .normal = vec3(first.normal),
                .speed = first.speed,
            });
        } else if (before && !now) {
            world.ended.push_back(entry->second.a);
            world.ended.push_back(entry->second.b);
        }
        if (!now && entry != world.touching.end()) {
            world.touching.erase(entry);
        }
    }
}

void forget_contacts(world_t& world, JPH::BodyID body) {
    const JPH::uint32 id = body.GetIndexAndSequenceNumber();
    std::erase_if(world.touching, [id](const auto& entry) {
        return static_cast<JPH::uint32>(entry.first >> 32U) == id ||
               static_cast<JPH::uint32>(entry.first) == id;
    });
}

} // namespace fbe::physics

extern "C" {

FBE_PHYSICS_API int32_t fbe_physics_began(uint64_t world, fbe_physics_contact_t* contacts,
                                          int32_t capacity) {
    using namespace fbe::physics;
    const fp_mode_t fp_mode;
    const world_t* found = find_world(world);
    if (found == nullptr) {
        return -1;
    }
    const auto count =
        std::min(found->began.size(), static_cast<std::size_t>(std::max(capacity, 0)));
    std::copy_n(found->began.begin(), count, contacts);
    return static_cast<int32_t>(found->began.size());
}

FBE_PHYSICS_API int32_t fbe_physics_ended(uint64_t world, uint64_t* pairs, int32_t capacity) {
    using namespace fbe::physics;
    const world_t* found = find_world(world);
    if (found == nullptr) {
        return -1;
    }
    const std::size_t count = found->ended.size() / 2;
    const auto copied = std::min(count, static_cast<std::size_t>(std::max(capacity, 0)));
    std::copy_n(found->ended.begin(), copied * 2, pairs);
    return static_cast<int32_t>(count);
}

} // extern "C"
