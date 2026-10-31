// Characters: Jolt's virtual characters, by handle, each in its world's list, with an inner body
// that the body pool knows as a character's, so the game reads it but never moves or frees it.

#include "fbengine_physics.h"
#include "physics/physics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <memory>
#include <numbers>
#include <vector>

namespace {

using namespace fbe::physics;

// Jolt's own: the highest step, which keeping to the floor goes a quarter further down than.
constexpr float default_step_height = 0.4F;
constexpr float stick_down_of_step = 1.25F;

struct character_t {
    world_t* world;
    JPH::Ref<JPH::CharacterVirtual> character;
    std::uint64_t body;
    // Where it is in its world's list.
    std::size_t index;
    float step_height = default_step_height;
};

pool_t<character_t>& characters() {
    // Never freed, as Jolt isn't: statics freed as the process ends go in any order, and a world
    // left then would free its bodies into a pool that had gone.
    static auto* pool = new pool_t<character_t>(CHARACTER_KIND);
    return *pool;
}

character_t* find(std::uint64_t handle) {
    character_t* character = characters().get(handle);
    if (character == nullptr) {
        fail(handle == 0 ? "there's no character" : "the character was freed");
    }
    return character;
}

bool finite(const fbe_character_vec3_t& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

JPH::Vec3 vec3(const fbe_character_vec3_t& v) {
    return { v.x, v.y, v.z };
}

fbe_character_vec3_t vec3(JPH::Vec3Arg v) {
    return { .x = v.GetX(), .y = v.GetY(), .z = v.GetZ() };
}

// A shape a character can be of: one, with no mesh in it.
const JPH::Shape* character_shape(std::uint64_t shape) {
    const JPH::Shape* found = find_shape(shape);
    if (found != nullptr && found->MustBeStatic()) {
        fail("a character's shape has no mesh in it");
        return nullptr;
    }
    return found;
}

// What a character collides with: what its body's layer does, but its body.
struct filters_t {
    explicit filters_t(const character_t& c)
        : layer(c.world->system.GetBodyLockInterfaceNoLock()
                    .TryGetBody(c.character->GetInnerBodyID())
                    ->GetObjectLayer()),
          broad_phase(c.world->system.GetDefaultBroadPhaseLayerFilter(layer)),
          objects(c.world->system.GetDefaultLayerFilter(layer)),
          bodies(c.character->GetInnerBodyID()) {}

    JPH::ObjectLayer layer;
    JPH::DefaultBroadPhaseLayerFilter broad_phase;
    JPH::DefaultObjectLayerFilter objects;
    JPH::IgnoreSingleBodyFilter bodies;
    JPH::ShapeFilter shapes;
};

void remove(std::uint64_t handle) {
    character_t* found = characters().get(handle);
    world_t& world = *found->world;
    world.characters_vs.Remove(found->character);
    drop_character_body(found->body);
    // Jolt's character takes its body out of the world as it goes.
    found->character = nullptr;
    const std::uint64_t last = world.characters.back();
    world.characters[found->index] = last;
    characters().get(last)->index = found->index;
    world.characters.pop_back();
    characters().remove(handle);
}

// A float of a character's, by `get`, or -1.
template <typename Get>
float get_float(std::uint64_t handle, Get get) {
    const character_t* found = find(handle);
    return found != nullptr ? get(*found) : -1.0F;
}

// Sets a float of a character's by `set` if it's finite and `in_range`; false, saying `range`, if
// it isn't.
template <typename Set>
bool set_float(std::uint64_t handle, float value, bool in_range, const char* range, Set set) {
    character_t* found = find(handle);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(value) || !in_range) {
        return fail(std::format("{}, not {}", range, value));
    }
    set(*found, value);
    return true;
}

} // namespace

namespace fbe::physics {

void free_characters(world_t& world) {
    while (!world.characters.empty()) {
        remove(world.characters.back());
    }
}

void save_characters(const world_t& world, JPH::StateRecorder& state) {
    std::vector<std::uint64_t> handles = world.characters;
    std::ranges::sort(handles);
    for (const std::uint64_t handle : handles) {
        state.Write(handle);
        characters().get(handle)->character->SaveState(state);
    }
}

bool restore_characters(const world_t& world, JPH::StateRecorder& state) {
    std::vector<std::uint64_t> handles = world.characters;
    std::ranges::sort(handles);
    for (const std::uint64_t handle : handles) {
        std::uint64_t saved = 0;
        state.Read(saved);
        if (failed(state) || saved != handle) {
            return false;
        }
        characters().get(handle)->character->RestoreState(state);
    }
    return !failed(state);
}

} // namespace fbe::physics

extern "C" {

FBE_PHYSICS_API uint64_t fbe_character_new(uint64_t world, uint64_t shape,
                                           const fbe_character_vec3_t* position) {
    world_t* in = find_world(world);
    if (in == nullptr) {
        return 0;
    }
    const JPH::Shape* of = character_shape(shape);
    if (of == nullptr) {
        return 0;
    }
    if (!finite(*position)) {
        fail("a character's position is finite");
        return 0;
    }
    if (in->bodies.size() >= in->system.GetMaxBodies()) {
        fail(std::format("the world has room for {} bodies, and has them all",
                         in->system.GetMaxBodies()));
        return 0;
    }
    JPH::CharacterVirtualSettings settings;
    settings.mShape = of;
    settings.mInnerBodyShape = of;
    settings.mInnerBodyLayer = object_layer(0, true);
    settings.mMaxSlopeAngle = JPH::DegreesToRadians(50.0F);
    const JPH::Ref<JPH::CharacterVirtual> character = new JPH::CharacterVirtual(
        &settings, vec3(*position), JPH::Quat::sIdentity(), 0, &in->system);
    if (character->GetInnerBodyID().IsInvalid()) {
        fail(std::format("the world has room for {} bodies, and has them all",
                         in->system.GetMaxBodies()));
        return 0;
    }
    const std::uint64_t body = add_character_body(*in, character->GetInnerBodyID());
    character->SetUserData(body);
    character->SetCharacterVsCharacterCollision(&in->characters_vs);
    in->characters_vs.Add(character);
    changed(*in);
    const std::uint64_t handle =
        characters().add(std::make_unique<character_t>(in, character, body, in->characters.size()));
    in->characters.push_back(handle);
    return handle;
}

FBE_PHYSICS_API bool fbe_character_free(uint64_t character) {
    if (find(character) == nullptr) {
        return false;
    }
    world_t& world = *characters().get(character)->world;
    remove(character);
    changed(world);
    return true;
}

FBE_PHYSICS_API uint64_t fbe_character_body(uint64_t character) {
    const character_t* found = find(character);
    return found != nullptr ? found->body : 0;
}

FBE_PHYSICS_API bool fbe_character_move(uint64_t character, const fbe_character_vec3_t* velocity,
                                        float seconds) {
    const character_t* found = find(character);
    if (found == nullptr) {
        return false;
    }
    if (!finite(*velocity)) {
        return fail("a character moves at a finite velocity");
    }
    if (!std::isfinite(seconds) || seconds <= 0.0F || seconds > 1.0F) {
        return fail(std::format("a character moves for more than 0 seconds and at most 1, not {}",
                                seconds));
    }
    JPH::CharacterVirtual& moving = *found->character;
    moving.SetLinearVelocity(vec3(*velocity));
    JPH::CharacterVirtual::ExtendedUpdateSettings update;
    update.mWalkStairsStepUp = JPH::Vec3(0.0F, found->step_height, 0.0F);
    update.mStickToFloorStepDown = JPH::Vec3(0.0F, -found->step_height * stick_down_of_step, 0.0F);
    const filters_t filters(*found);
    moving.ExtendedUpdate(seconds, found->world->system.GetGravity(), update, filters.broad_phase,
                          filters.objects, filters.bodies, filters.shapes, step_memory());
    return true;
}

FBE_PHYSICS_API bool fbe_character_position(uint64_t character, fbe_character_vec3_t* position) {
    const character_t* found = find(character);
    if (found == nullptr) {
        return false;
    }
    *position = vec3(JPH::Vec3(found->character->GetPosition()));
    return true;
}

FBE_PHYSICS_API bool fbe_character_set_position(uint64_t character,
                                                const fbe_character_vec3_t* position) {
    const character_t* found = find(character);
    if (found == nullptr) {
        return false;
    }
    if (!finite(*position)) {
        return fail("a character's position is finite");
    }
    found->character->SetPosition(vec3(*position));
    return true;
}

FBE_PHYSICS_API bool fbe_character_velocity(uint64_t character, fbe_character_vec3_t* velocity) {
    const character_t* found = find(character);
    if (found == nullptr) {
        return false;
    }
    *velocity = vec3(found->character->GetLinearVelocity());
    return true;
}

FBE_PHYSICS_API int32_t fbe_character_ground(uint64_t character) {
    const character_t* found = find(character);
    if (found == nullptr) {
        return -1;
    }
    switch (found->character->GetGroundState()) {
    case JPH::CharacterBase::EGroundState::OnGround:
        return FBE_CHARACTER_ON_GROUND;
    case JPH::CharacterBase::EGroundState::OnSteepGround:
        return FBE_CHARACTER_ON_STEEP_GROUND;
    case JPH::CharacterBase::EGroundState::NotSupported:
        return FBE_CHARACTER_NOT_SUPPORTED;
    case JPH::CharacterBase::EGroundState::InAir:
        return FBE_CHARACTER_IN_AIR;
    }
    return -1;
}

FBE_PHYSICS_API bool fbe_character_ground_normal(uint64_t character, fbe_character_vec3_t* normal) {
    const character_t* found = find(character);
    if (found == nullptr) {
        return false;
    }
    *normal = vec3(found->character->GetGroundNormal());
    return true;
}

FBE_PHYSICS_API bool fbe_character_ground_velocity(uint64_t character,
                                                   fbe_character_vec3_t* velocity) {
    const character_t* found = find(character);
    if (found == nullptr) {
        return false;
    }
    found->character->UpdateGroundVelocity();
    *velocity = vec3(found->character->GetGroundVelocity());
    return true;
}

FBE_PHYSICS_API uint64_t fbe_character_ground_body(uint64_t character) {
    const character_t* found = find(character);
    if (found == nullptr) {
        return 0;
    }
    const JPH::BodyID ground = found->character->GetGroundBodyID();
    const JPH::Body* body =
        ground.IsInvalid() ? nullptr
                           : found->world->system.GetBodyLockInterfaceNoLock().TryGetBody(ground);
    return body != nullptr ? body->GetUserData() : 0;
}

FBE_PHYSICS_API bool fbe_character_set_shape(uint64_t character, uint64_t shape) {
    const character_t* found = find(character);
    if (found == nullptr) {
        return false;
    }
    const JPH::Shape* of = character_shape(shape);
    if (of == nullptr) {
        return false;
    }
    const filters_t filters(*found);
    const float deepest = 1.5F * found->world->system.GetPhysicsSettings().mPenetrationSlop;
    if (!found->character->SetShape(of, deepest, filters.broad_phase, filters.objects,
                                    filters.bodies, filters.shapes, step_memory())) {
        return fail("the shape doesn't fit where the character is");
    }
    found->character->SetInnerBodyShape(of);
    return true;
}

FBE_PHYSICS_API float fbe_character_max_slope(uint64_t character) {
    return get_float(character, [](const character_t& c) {
        return std::acos(c.character->GetCosMaxSlopeAngle());
    });
}

FBE_PHYSICS_API float fbe_character_mass(uint64_t character) {
    return get_float(character, [](const character_t& c) { return c.character->GetMass(); });
}

FBE_PHYSICS_API float fbe_character_strength(uint64_t character) {
    return get_float(character, [](const character_t& c) { return c.character->GetMaxStrength(); });
}

FBE_PHYSICS_API float fbe_character_step_height(uint64_t character) {
    return get_float(character, [](const character_t& c) { return c.step_height; });
}

FBE_PHYSICS_API bool fbe_character_set_max_slope(uint64_t character, float radians) {
    return set_float(character, radians,
                     radians >= 0.0F && radians <= std::numbers::pi_v<float> / 2,
                     "a max slope is 0 to pi / 2",
                     [](character_t& c, float v) { c.character->SetMaxSlopeAngle(v); });
}

FBE_PHYSICS_API bool fbe_character_set_mass(uint64_t character, float mass) {
    return set_float(character, mass, mass > 0.0F, "a character's mass is more than 0",
                     [](character_t& c, float v) { c.character->SetMass(v); });
}

FBE_PHYSICS_API bool fbe_character_set_strength(uint64_t character, float strength) {
    return set_float(character, strength, strength > 0.0F, "a character's strength is more than 0",
                     [](character_t& c, float v) { c.character->SetMaxStrength(v); });
}

FBE_PHYSICS_API bool fbe_character_set_step_height(uint64_t character, float height) {
    return set_float(character, height, height >= 0.0F, "a step height is 0 or more",
                     [](character_t& c, float v) { c.step_height = v; });
}

FBE_PHYSICS_API int32_t fbe_character_count(void) {
    return static_cast<int32_t>(characters().count());
}

} // extern "C"
