// Particles' and decals' C API with each of a call's allocations failing in turn, built with
// fbengine's sources, whose operator new fails the allocation it's told to: nothing a call throws
// gets out of it, a call that fails says "out of memory", and what it was called on works after.

#include "fbengine.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <new>
#include <string_view>

namespace {

/// Which of this thread's allocations fails, -1 for none, how many there have been, and whether
/// one failed.
thread_local long failing = -1;
thread_local long allocated = 0;
thread_local bool failed = false;

} // namespace

// As tests/anim/oom_tests.cpp's: new[] and the nothrow forms are this test's too, and the default
// deletes free what they give, as malloc's.
// NOLINTBEGIN(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
// NOLINTBEGIN(misc-new-delete-overloads)
void* operator new(std::size_t size) {
    if (failing >= 0 && allocated++ == failing) {
        failing = -1;
        failed = true;
        throw std::bad_alloc();
    }
    void* made = std::malloc(size != 0 ? size : 1);
    if (made == nullptr) {
        throw std::bad_alloc();
    }
    return made;
}

void* operator new[](std::size_t size) {
    return operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t& /*nothrow*/) noexcept {
    try {
        return operator new(size);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t& /*nothrow*/) noexcept {
    try {
        return operator new(size);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}
// NOLINTEND(misc-new-delete-overloads)
// NOLINTEND(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)

namespace {

/// One of an Instances' at a place along X.
fbe_instance_t instance_at(float x) {
    fbe_instance_t made{};
    made.matrix.m[0] = 1.0F;
    made.matrix.m[5] = 1.0F;
    made.matrix.m[10] = 1.0F;
    made.matrix.m[12] = x;
    made.matrix.m[15] = 1.0F;
    made.color = { .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F };
    return made;
}

/// What the calls work on: an emitter with some alive, and a cube a decal can go on.
struct rig_t {
    std::uint64_t world = fbe_world_create();
    std::uint64_t emitter = fbe_entity_create(world, 0, FBE_ENTITY_PARTICLES);
    std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    std::uint64_t geometry = fbe_geometry_cube();
    std::uint64_t instances = fbe_entity_create(world, 0, FBE_ENTITY_INSTANCES);
    std::uint64_t coarser = fbe_geometry_cube();
    std::uint64_t with_levels = fbe_geometry_cube();

    rig_t() {
        fbe_mesh_set_geometry(mesh, geometry);
        fbe_instances_set_geometry(instances, geometry);
        const std::array<fbe_instance_t, 2> two{ instance_at(1.0F), instance_at(2.0F) };
        fbe_instances_set(instances, two.data(), 2);
        fbe_geometry_set_lod(with_levels, 0, coarser, 0.5F);
        fbe_particles_set_rate(emitter, 100.0F);
        fbe_particles_set_life(emitter, 10.0F, 10.0F);
        fbe_particles_burst(emitter, 8);
    }

    rig_t(const rig_t&) = delete;
    rig_t& operator=(const rig_t&) = delete;
    rig_t(rig_t&&) = delete;
    rig_t& operator=(rig_t&&) = delete;

    ~rig_t() {
        fbe_geometry_release(with_levels);
        fbe_geometry_release(coarser);
        fbe_geometry_release(geometry);
        fbe_world_free(world);
    }
};

/// A box over the cube's +Z face.
fbe_mat4_t over_cube() {
    fbe_mat4_t box{};
    box.m[0] = 0.5F;
    box.m[5] = 0.5F;
    box.m[10] = 0.5F;
    box.m[14] = 1.0F;
    box.m[15] = 1.0F;
    return box;
}

void still_works(const rig_t& rig) {
    const std::int32_t before = fbe_particles_alive(rig.emitter);
    CHECK(fbe_particles_burst(rig.emitter, 1));
    CHECK(fbe_particles_alive(rig.emitter) >= std::min(before + 1, 16));
    CHECK(fbe_world_step_particles(rig.world, 0.1F));
    const fbe_mat4_t box = over_cube();
    const std::uint64_t made = fbe_geometry_decal(rig.world, &box, 0.0F, 1.3F);
    REQUIRE(made != 0);
    CHECK(fbe_surface_vertex_count(made, 0) > 0);
    fbe_geometry_release(made);
    // Its Instances are all there, in step, whatever failed.
    const std::int32_t count = fbe_instances_count(rig.instances);
    REQUIRE(count > 0);
    for (std::int32_t i = 0; i < count; ++i) {
        fbe_instance_t one{};
        CHECK(fbe_instances_get(rig.instances, i, &one));
    }
    fbe_aabb_t bounds{};
    CHECK(fbe_entity_bounds(rig.instances, &bounds));
    CHECK(fbe_geometry_lod_count(rig.with_levels) == 1);
}

/// Runs `call` on a new rig with its first allocation failing, then its second, and so on, until
/// one in which none did: each time it says so and gives false, and the rig works after.
void sweep(const std::function<bool(const rig_t&)>& call) {
    for (long at = 0; at < 10000; ++at) {
        const rig_t rig;
        allocated = 0;
        failed = false;
        failing = at;
        const bool done = call(rig);
        failing = -1;
        if (!failed) {
            CHECK(done);
            still_works(rig);
            return;
        }
        CHECK_FALSE(done);
        CHECK(std::string_view(fbe_error()) == "out of memory");
        still_works(rig);
    }
    FAIL("it never ran out of allocations to fail");
}

} // namespace

TEST_CASE("a step that runs out of memory says so", "[scene][oom]") {
    sweep([](const rig_t& rig) { return fbe_world_step_particles(rig.world, 0.5F); });
}

TEST_CASE("a burst that runs out of memory says so", "[scene][oom]") {
    sweep([](const rig_t& rig) { return fbe_particles_burst(rig.emitter, 64); });
}

TEST_CASE("setting what it holds out of memory leaves it as it was", "[scene][oom]") {
    sweep([](const rig_t& rig) {
        const bool done = fbe_particles_set_capacity(rig.emitter, 16);
        // It holds what it did, or 16, and either way writes in what it has.
        CHECK((fbe_particles_capacity(rig.emitter) == 16) == done);
        return done;
    });
}

TEST_CASE("an Instances set out of memory stays as it was", "[scene][oom]") {
    sweep([](const rig_t& rig) {
        std::array<fbe_instance_t, 200> all{};
        for (std::size_t i = 0; i < all.size(); ++i) {
            all[i] = instance_at(static_cast<float>(i));
        }
        const bool done = fbe_instances_set(rig.instances, all.data(), 200);
        CHECK(fbe_instances_count(rig.instances) == (done ? 200 : 2));
        return done;
    });
}

TEST_CASE("an Instances' bounds out of memory say so", "[scene][oom]") {
    sweep([](const rig_t& rig) {
        fbe_aabb_t box{};
        return fbe_entity_bounds(rig.instances, &box);
    });
}

TEST_CASE("one of an Instances set out of memory is set, and worked out with the rest",
          "[scene][oom]") {
    for (long at = 0; at < 4; ++at) {
        const rig_t rig;
        fbe_aabb_t box{};
        REQUIRE(fbe_entity_bounds(rig.instances, &box));
        allocated = 0;
        failing = at;
        const fbe_instance_t moved = instance_at(50.0F);
        const bool done = fbe_instances_set_one(rig.instances, 1, &moved);
        failing = -1;
        CHECK(done);
        REQUIRE(fbe_entity_bounds(rig.instances, &box));
        CHECK(box.max.x == 51.0F);
    }
}

TEST_CASE("a level of detail out of memory says so", "[scene][oom]") {
    sweep(
        [](const rig_t& rig) { return fbe_geometry_set_lod(rig.geometry, 0, rig.coarser, 0.5F); });
}

TEST_CASE("a copy of a geometry with levels out of memory says so", "[scene][oom]") {
    sweep([](const rig_t& rig) {
        const std::uint64_t made = fbe_geometry_clone(rig.with_levels);
        if (made == 0) {
            return false;
        }
        CHECK(fbe_geometry_lod_count(made) == 1);
        fbe_geometry_release(made);
        return true;
    });
}

TEST_CASE("a decal that runs out of memory says so, and leaves nothing", "[scene][oom]") {
    sweep([](const rig_t& rig) {
        const fbe_mat4_t box = over_cube();
        const std::uint64_t made = fbe_geometry_decal(rig.world, &box, 0.0F, 1.3F);
        if (made == 0) {
            return false;
        }
        CHECK(fbe_surface_vertex_count(made, 0) > 0);
        fbe_geometry_release(made);
        return true;
    });
}
