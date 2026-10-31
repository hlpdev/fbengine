// Handles, as native/common/handles.hpp makes them for every package, and the scene's pools
// refusing each other's: a handle of one kind is never taken for another's, whatever its slot and
// generation.

#include "../../native/common/handles.hpp"
#include "fbengine.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string_view>

TEST_CASE("a pool refuses another kind's handle of its own slot and generation", "[scene]") {
    fbe::handle_slots_t worlds(fbe::WORLD_KIND);
    fbe::handle_slots_t entities(fbe::ENTITY_KIND);
    const std::uint32_t world = worlds.make();
    const std::uint32_t entity = entities.make();
    REQUIRE(world == entity);
    // The same slot and generation, but each pool's own kind.
    CHECK((worlds.handle(world) & 0x00FFFFFFFFFFFFFFULL) ==
          (entities.handle(entity) & 0x00FFFFFFFFFFFFFFULL));
    CHECK(worlds.handle(world) != entities.handle(entity));
    CHECK(worlds.find(worlds.handle(world)) == world);
    CHECK_FALSE(worlds.find(entities.handle(entity)));
    CHECK_FALSE(entities.find(worlds.handle(world)));
    CHECK(fbe::kind_of(worlds.handle(world)) == fbe::WORLD_KIND);
}

TEST_CASE("a slot freed at its last generation is retired", "[scene]") {
    fbe::handle_slots_t slots(fbe::GEOMETRY_KIND);
    std::uint32_t slot = slots.make();
    for (std::uint32_t generation = 1; generation < fbe::handle_slots_t::last_generation;
         ++generation) {
        slots.free(slot);
        slot = slots.make();
    }
    // Its generation is the last now; freeing it once more retires it, so a new slot is made.
    const std::uint64_t last = slots.handle(slot);
    CHECK(((last >> 32U) & 0xFFFFFFU) == fbe::handle_slots_t::last_generation);
    slots.free(slot);
    CHECK_FALSE(slots.find(last));
    CHECK(slots.make() != slot);
}

TEST_CASE("the scene's pools refuse each other's handles, both ways", "[scene]") {
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t entity = fbe_entity_create(world, 0, FBE_ENTITY_PIVOT);
    const std::uint64_t geometry = fbe_geometry_create();
    const std::uint64_t material = fbe_material_create();
    // Each kind's handle, with its slot and generation given another kind.
    const auto as_kind = [](std::uint64_t handle, std::uint8_t kind) {
        return (handle & 0x00FFFFFFFFFFFFFFULL) | (static_cast<std::uint64_t>(kind) << 56U);
    };
    CHECK_FALSE(fbe_entity_alive(world));
    CHECK_FALSE(fbe_entity_free(world));
    CHECK(std::string_view(fbe_error()) == "that handle isn't an entity's");
    CHECK_FALSE(fbe_world_alive(entity));
    CHECK_FALSE(fbe_world_clear(entity));
    CHECK(std::string_view(fbe_error()) == "that handle isn't a World's");
    CHECK_FALSE(fbe_entity_alive(geometry));
    CHECK_FALSE(fbe_geometry_alive(entity));
    CHECK(fbe_surface_vertex_count(entity, 0) == -1);
    CHECK(std::string_view(fbe_error()) == "that handle isn't a geometry's");
    CHECK_FALSE(fbe_material_alive(geometry));
    CHECK_FALSE(fbe_geometry_alive(material));
    CHECK_FALSE(fbe_entity_alive(as_kind(entity, fbe::GEOMETRY_KIND)));
    CHECK_FALSE(fbe_geometry_alive(as_kind(geometry, fbe::ENTITY_KIND)));
    CHECK(fbe_entity_alive(as_kind(entity, fbe::ENTITY_KIND)));
    fbe_geometry_release(geometry);
    fbe_material_release(material);
    fbe_world_free(world);
    std::array<std::uint64_t, 4> some{};
    while (fbe_entity_freed(some.data(), some.size()) != 0) {}
}
