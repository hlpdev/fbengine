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

/// What the calls work on: an emitter with some alive, and a cube a decal can go on.
struct rig_t {
    std::uint64_t world = fbe_world_create();
    std::uint64_t emitter = fbe_entity_create(world, 0, FBE_ENTITY_PARTICLES);
    std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    std::uint64_t geometry = fbe_geometry_cube();

    rig_t() {
        fbe_mesh_set_geometry(mesh, geometry);
        fbe_particles_set_rate(emitter, 100.0F);
        fbe_particles_set_life(emitter, 10.0F, 10.0F);
        fbe_particles_burst(emitter, 8);
    }

    rig_t(const rig_t&) = delete;
    rig_t& operator=(const rig_t&) = delete;
    rig_t(rig_t&&) = delete;
    rig_t& operator=(rig_t&&) = delete;

    ~rig_t() {
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
