// Animation's C API with each of a call's allocations failing in turn, built with fbengine's
// sources, whose operator new fails the allocation it's told to: nothing a call throws gets out of
// it, a call that fails says "out of memory", and what it was called on works after.

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

// As tests/audio/oom_tests.cpp's: new[] and the nothrow forms are this test's too, and the default
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

constexpr std::array<float, 2> times{ 0.0F, 1.0F };
constexpr std::array<float, 6> places{ 0.0F, 0.0F, 0.0F, 2.0F, 0.0F, 0.0F };
constexpr std::array<float, 16> identity{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

/// What the calls work on, made afresh for each allocation failing: a body with a mesh on a surface
/// of a vertex, hips, two clips of the hips, one playing, and a skin of the hips.
struct rig_t {
    std::uint64_t world = fbe_world_create();
    std::uint64_t body = fbe_entity_create(world, 0, FBE_ENTITY_PIVOT);
    std::uint64_t mesh = fbe_entity_create(world, body, FBE_ENTITY_MESH);
    std::uint64_t hips = fbe_entity_create(world, body, FBE_ENTITY_PIVOT);
    std::uint64_t geometry = fbe_geometry_create();
    std::uint64_t walk = fbe_clip_create("walk", 0.0F);
    std::uint64_t run = fbe_clip_create("run", 0.0F);
    std::uint64_t skin = fbe_skin_create();
    std::uint64_t player = fbe_player_create(body);

    rig_t() {
        fbe_entity_set_name(hips, "hips");
        fbe_geometry_add_surface(geometry);
        const std::array<fbe_vertex_t, 1> vertex{};
        fbe_surface_add_vertices(geometry, 0, vertex.data(), 1);
        fbe_mesh_set_geometry(mesh, geometry);
        fbe_clip_add_channel(walk, "hips", FBE_CLIP_TRANSLATION, FBE_CLIP_LINEAR, 3, times.data(),
                             places.data(), 2);
        fbe_clip_add_event(walk, "step", 0.5F);
        fbe_clip_add_channel(run, "hips", FBE_CLIP_TRANSLATION, FBE_CLIP_LINEAR, 3, times.data(),
                             places.data(), 2);
        fbe_skin_add_joint(skin, "hips", identity.data());
        fbe_player_play(player, 0, walk, FBE_PLAYER_LOOP, 1.0F);
    }

    rig_t(const rig_t&) = delete;
    rig_t& operator=(const rig_t&) = delete;
    rig_t(rig_t&&) = delete;
    rig_t& operator=(rig_t&&) = delete;

    ~rig_t() {
        fbe_player_free(player);
        fbe_clip_release(walk);
        fbe_clip_release(run);
        fbe_skin_release(skin);
        fbe_geometry_release(geometry);
        fbe_world_free(world);
    }
};

/// The rig works: its player moves on and poses, a clip can be made, and so can a skin's palette.
void still_works(const rig_t& rig) {
    CHECK(fbe_player_set_time(rig.player, 0, 0.5F));
    CHECK(fbe_player_advance(rig.world, 0.25F));
    fbe_player_clear_events(rig.player);
    fbe_vec3_t at{};
    CHECK(fbe_entity_position(rig.hips, FBE_SPACE_LOCAL, &at));
    CHECK(at.x > 0.0F);
    const std::uint64_t clip = fbe_clip_create("again", 0.0F);
    CHECK(clip != 0);
    CHECK(fbe_clip_add_channel(clip, "hips", FBE_CLIP_SCALE, FBE_CLIP_LINEAR, 3, times.data(),
                               places.data(), 2));
    CHECK(fbe_clip_channel_count(clip) == 1);
    CHECK(fbe_clip_release(clip));
    CHECK(fbe_skin_attach(rig.mesh, rig.skin, rig.body));
    std::array<float, 32> palette{};
    CHECK(fbe_skin_palette(rig.mesh, palette.data(), 2) == fbe_skin_joint_count(rig.skin));
}

/// Runs `call` on a new rig, `first` having run on it, with its first allocation failing, then its
/// second, and so on, until one in which none did: each time, it says so and gives false, and the
/// rig works after.
void sweep(
    const std::function<bool(const rig_t&)>& call,
    const std::function<void(const rig_t&)>& first = [](const rig_t&) {}) {
    for (long which = 0; which < 2000; ++which) {
        const rig_t rig;
        first(rig);
        allocated = 0;
        failed = false;
        failing = which;
        const bool done = call(rig);
        failing = -1;
        if (!failed) {
            CHECK(done);
            still_works(rig);
            return;
        }
        CHECK(!done);
        CHECK(std::string_view(fbe_error()).contains("out of memory"));
        still_works(rig);
    }
    FAIL("a call never got to the end of its allocations");
}

} // namespace

TEST_CASE("clips' calls that run out of memory say so, and leave things working") {
    sweep([](const rig_t&) {
        const std::uint64_t made = fbe_clip_create("a name long enough to be allocated", 0.0F);
        return made != 0 && fbe_clip_release(made);
    });
    sweep([](const rig_t& rig) {
        return fbe_clip_add_channel(rig.run, "a node long enough to be allocated",
                                    FBE_CLIP_TRANSLATION, FBE_CLIP_LINEAR, 3, times.data(),
                                    places.data(), 2);
    });
    sweep([](const rig_t& rig) {
        return fbe_clip_add_event(rig.run, "an event long enough to be allocated", 0.25F);
    });
    sweep([](const rig_t& rig) {
        const std::uint64_t made = fbe_clip_slice(rig.walk, 0.25F, 0.75F);
        return made != 0 && fbe_clip_release(made);
    });
}

TEST_CASE("skins' calls that run out of memory say so, and leave things working") {
    sweep([](const rig_t&) {
        const std::uint64_t made = fbe_skin_create();
        return made != 0 && fbe_skin_release(made);
    });
    sweep([](const rig_t& rig) {
        return fbe_skin_add_joint(rig.skin, "a joint long enough to be allocated", identity.data());
    });
    sweep([](const rig_t& rig) { return fbe_skin_attach(rig.mesh, rig.skin, rig.body); });
    sweep(
        [](const rig_t& rig) {
            std::array<float, 16> palette{};
            return fbe_skin_palette(rig.mesh, palette.data(), 1) == 1;
        },
        [](const rig_t& rig) { fbe_skin_attach(rig.mesh, rig.skin, rig.body); });
}

TEST_CASE("players' calls that run out of memory say so, and leave things working") {
    sweep([](const rig_t& rig) {
        const std::uint64_t made = fbe_player_create(rig.hips);
        return made != 0 && fbe_player_free(made);
    });
    sweep([](const rig_t& rig) {
        return fbe_player_play(rig.player, 1, rig.run, FBE_PLAYER_LOOP, 1.0F);
    });
    sweep([](const rig_t& rig) {
        return fbe_player_crossfade(rig.player, 0, rig.run, 1.0F, FBE_PLAYER_LOOP, 1.0F);
    });
    sweep([](const rig_t& rig) { return fbe_player_set_time(rig.player, 0, 0.75F); });
    sweep([](const rig_t& rig) { return fbe_player_advance(rig.world, 0.6F); });
    sweep([](const rig_t& rig) {
        const std::array<std::uint64_t, 1> mask{ rig.hips };
        return fbe_player_set_mask(rig.player, 0, mask.data(), 1);
    });
    sweep([](const rig_t& rig) { return fbe_player_release(rig.player, rig.mesh); });
}

TEST_CASE("morph targets' calls that run out of memory say so, and leave things working") {
    sweep([](const rig_t& rig) {
        const std::array<float, 3> moved{ 1.0F, 0.0F, 0.0F };
        return fbe_morph_add_target(rig.geometry, 0, moved.data(), moved.data(), 1) == 0;
    });
    sweep([](const rig_t& rig) {
        const std::array<float, 8> weights{};
        return fbe_morph_set_weights(rig.mesh, weights.data(), weights.size());
    });
    sweep([](const rig_t& rig) { return fbe_world_capture(rig.world); },
          [](const rig_t& rig) {
              const std::array<float, 8> weights{};
              fbe_morph_set_weights(rig.mesh, weights.data(), weights.size());
          });
}
