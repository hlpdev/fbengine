// Animation's C API with each of a call's allocations failing in turn, built with fbengine's
// sources, whose operator new fails the allocation it's told to: nothing a call throws gets out of
// it, a call that fails says "out of memory", and what it was called on works after.

#include "core/engine.hpp"
#include "fbengine.h"
#include "gpu/buffer.hpp"
#include "gpu/vulkan.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

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

TEST_CASE("shaders' calls that run out of memory say so, and leave things working") {
    // SPIR-V's header, and the scene's entry points.
    std::vector<std::uint32_t> words{ 0x07230203, 0x00010500, 0, 1, 0 };
    for (const auto& [model, name] : std::array<std::pair<std::uint32_t, std::string_view>, 5>{
             {
                 { 0, "vertexMain" },
                 { 4, "fragmentMain" },
                 { 0, "particleVertex" },
                 { 0, "shadowVertex" },
                 { 4, "shadowFragment" },
             },
         }) {
        const std::size_t name_words = (name.size() / 4) + 1;
        words.push_back(static_cast<std::uint32_t>((3 + name_words) << 16U) | 15U);
        words.push_back(model);
        words.push_back(1);
        std::vector<std::uint32_t> named(name_words, 0);
        std::memcpy(named.data(), name.data(), name.size());
        words.insert(words.end(), named.begin(), named.end());
    }
    const std::string_view magic = "FBESHADR";
    const std::string_view version = fbe_version();
    const std::string_view stamp = fbe_shader_stamp();
    std::vector<std::uint8_t> bytes(magic.begin(), magic.end());
    const auto length = static_cast<std::uint32_t>(version.size());
    bytes.resize(bytes.size() + sizeof(length));
    std::memcpy(bytes.data() + magic.size(), &length, sizeof(length));
    bytes.insert(bytes.end(), version.begin(), version.end());
    bytes.insert(bytes.end(), stamp.begin(), stamp.end());
    const std::size_t at = bytes.size();
    bytes.resize(at + (words.size() * sizeof(std::uint32_t)));
    std::memcpy(bytes.data() + at, words.data(), words.size() * sizeof(std::uint32_t));
    const auto size = static_cast<std::int64_t>(bytes.size());
    sweep([&](const rig_t&) {
        const std::uint64_t made =
            fbe_shader_create(bytes.data(), size, "a name long enough to be allocated");
        return made != 0 && fbe_shader_release(made);
    });
    sweep([&](const rig_t&) { return fbe_shader_check(bytes.data(), size); });
    // What's made after the sweep's slots and places are where they should be.
    const std::uint64_t made = fbe_shader_create(bytes.data(), size, "after");
    CHECK(made != 0);
    CHECK(fbe_shader_release(made));
}

TEST_CASE("an index that isn't there, out of memory, says so without throwing") {
    const std::uint64_t material = fbe_material_create();
    std::array<float, 4> read{};
    for (long which = 0; which < 8; ++which) {
        allocated = 0;
        failed = false;
        failing = which;
        const bool set = fbe_material_set_parameter(material, 99, 0.0F, 0.0F, 0.0F, 0.0F);
        const bool got = fbe_material_parameter(material, 99, read.data());
        const bool layer = fbe_material_set_texture(material, 99, 0);
        failing = -1;
        CHECK_FALSE(set);
        CHECK_FALSE(got);
        CHECK_FALSE(layer);
    }
    fbe_material_release(material);
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

namespace {

/// A clip of the first `count` targets of the mesh "face", and the rig's mesh named that.
void face_clip(const rig_t& rig) {
    fbe_entity_set_name(rig.mesh, "face");
    const std::array<float, 8> held{};
    const std::array<float, 1> at{ 0.0F };
    fbe_clip_add_channel(rig.run, "face", FBE_CLIP_WEIGHTS, FBE_CLIP_STEP, held.size(), at.data(),
                         held.data(), 1);
}

/// `call` starts rig.run on `layer`, as sweep runs it: while it hasn't installed it, the layer's as
/// it was; a pose that fails after leaves it installed. Whichever, nothing else holds the clip once
/// the player's freed.
void sweep_starting(
    std::int32_t layer, const std::function<bool(const rig_t&)>& call,
    const std::function<void(const rig_t&)>& first = [](const rig_t&) {}) {
    for (long which = 0; which < 2000; ++which) {
        const rig_t rig;
        first(rig);
        const std::uint64_t before = fbe_player_clip(rig.player, layer);
        allocated = 0;
        failed = false;
        failing = which;
        const bool done = call(rig);
        failing = -1;
        const std::uint64_t now = fbe_player_clip(rig.player, layer);
        if (!failed) {
            CHECK(done);
            CHECK(now == rig.run);
            return;
        }
        CHECK(!done);
        CHECK((now == before || now == rig.run));
        CHECK(fbe_player_advance(rig.world, 0.1F));
        CHECK(fbe_player_clip(rig.player, layer) == now);
        CHECK(fbe_player_free(rig.player));
        CHECK(fbe_clip_release(rig.run));
        CHECK_FALSE(fbe_clip_alive(rig.run));
    }
    FAIL("a call never got to the end of its allocations");
}

} // namespace

TEST_CASE("a clip that runs out of memory starting leaves the layer, and isn't held") {
    const auto starts = [](std::int32_t layer) {
        const auto play = [=](const rig_t& rig) {
            return fbe_player_play(rig.player, layer, rig.run, FBE_PLAYER_LOOP, 1.0F);
        };
        const auto crossfade = [=](const rig_t& rig) {
            return fbe_player_crossfade(rig.player, layer, rig.run, 1.0F, FBE_PLAYER_LOOP, 1.0F);
        };
        sweep_starting(layer, play);
        sweep_starting(layer, crossfade);
        sweep_starting(layer, play, face_clip);
        sweep_starting(layer, crossfade, face_clip);
    };
    // One that's playing, and one that's never played, with no room for a track yet.
    starts(0);
    starts(1);
}

TEST_CASE("a pose of weights that runs out of memory says so, and leaves things working") {
    sweep(
        [](const rig_t& rig) {
            return fbe_player_play(rig.player, 1, rig.run, FBE_PLAYER_LOOP, 1.0F);
        },
        face_clip);
    sweep([](const rig_t& rig) { return fbe_player_advance(rig.world, 0.1F); },
          [](const rig_t& rig) {
              face_clip(rig);
              fbe_player_play(rig.player, 1, rig.run, FBE_PLAYER_LOOP, 1.0F);
          });
}

TEST_CASE("a pose of weights out of memory poses none of what it moves") {
    for (long which = 0; which < 2000; ++which) {
        const rig_t rig;
        fbe_entity_set_name(rig.mesh, "first");
        const std::uint64_t second = fbe_entity_create(rig.world, rig.body, FBE_ENTITY_MESH);
        fbe_entity_set_name(second, "second");
        const std::array<float, 8> held{ 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F };
        const std::array<float, 1> at{ 0.0F };
        for (const char* node : { "first", "second" }) {
            fbe_clip_add_channel(rig.run, node, FBE_CLIP_WEIGHTS, FBE_CLIP_STEP, held.size(),
                                 at.data(), held.data(), 1);
        }
        allocated = 0;
        failed = false;
        failing = which;
        const bool done = fbe_player_play(rig.player, 1, rig.run, FBE_PLAYER_LOOP, 1.0F);
        failing = -1;
        const std::int32_t first_has = fbe_morph_weights(rig.mesh, 1.0F, nullptr, 0);
        const std::int32_t second_has = fbe_morph_weights(second, 1.0F, nullptr, 0);
        if (!failed) {
            CHECK(done);
            CHECK(first_has == 8);
            CHECK(second_has == 8);
            return;
        }
        CHECK(!done);
        CHECK(first_has == 0);
        CHECK(second_has == 0);
    }
    FAIL("a call never got to the end of its allocations");
}

TEST_CASE("a time set that runs out of memory leaves the time as it was") {
    for (long which = 0; which < 2000; ++which) {
        const rig_t rig;
        fbe_player_set_time(rig.player, 0, 0.25F);
        // A channel added has every track bind again in the next pose, which allocates.
        fbe_clip_add_channel(rig.walk, "hips", FBE_CLIP_SCALE, FBE_CLIP_LINEAR, 3, times.data(),
                             places.data(), 2);
        allocated = 0;
        failed = false;
        failing = which;
        const bool done = fbe_player_set_time(rig.player, 0, 0.75F);
        failing = -1;
        if (!failed) {
            CHECK(which > 0);
            CHECK(done);
            CHECK(fbe_player_time(rig.player, 0) == 0.75F);
            return;
        }
        CHECK(!done);
        CHECK(fbe_player_time(rig.player, 0) == 0.25F);
    }
    FAIL("a call never got to the end of its allocations");
}

namespace {

/// How many allocations a mesh's freeing makes, with another mesh, which moves into its place,
/// holding `weights`.
long allocations_freeing_with(std::span<const float> weights) {
    const rig_t rig;
    const std::uint64_t other = fbe_entity_create(rig.world, 0, FBE_ENTITY_MESH);
    fbe_morph_set_weights(other, weights.data(), static_cast<std::int32_t>(weights.size()));
    allocated = 0;
    failing = 1000000;
    CHECK(fbe_entity_free(rig.mesh));
    failing = -1;
    return allocated;
}

} // namespace

TEST_CASE("freeing a mesh doesn't allocate what it can run out of") {
    for (long which = 0; which < 50; ++which) {
        const rig_t rig;
        const std::uint64_t other = fbe_entity_create(rig.world, 0, FBE_ENTITY_MESH);
        const std::array<float, 8> weights{ 1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F };
        fbe_morph_set_weights(other, weights.data(), weights.size());
        allocated = 0;
        failed = false;
        failing = which;
        const bool done = fbe_entity_free(rig.mesh);
        failing = -1;
        if (!failed) {
            CHECK(done);
            std::array<float, 8> out{};
            CHECK(fbe_morph_weights(other, 1.0F, out.data(), out.size()) == 8);
            CHECK(out == weights);
            // What the mesh moved into its place holds isn't copied.
            CHECK(allocations_freeing_with({}) == allocations_freeing_with(weights));
            return;
        }
        CHECK(!done);
        CHECK(fbe_entity_alive(rig.mesh));
    }
    FAIL("a call never got to the end of its allocations");
}

#if defined(__linux__) || defined(__APPLE__)
// What's failing is this test's own: a library opened at run time, as a driver or a validation
// layer is, gets its C++ library's operator new, and throwing inside one, which it can't survive,
// is no test of the engine. Nor does it get what's instantiated here and calls that, as std::string
// is by GCC 13. Windows' DLLs have their own already. As own_new.map and own_new.exports have it.
TEST_CASE("what's opened at run time keeps its own operator new, and what calls it") {
    using new_t = void* (*)(std::size_t);
    Dl_info mine{};
    REQUIRE(dladdr(reinterpret_cast<void*>(static_cast<new_t>(&::operator new)), &mine) != 0);
    for (const char* name : {
             "_Znwm",
             "_Znam",
             "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE9_M_createERmm",
             "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE9_M_mutateEmmPKcm",
         }) {
        INFO(name);
        void* found = dlsym(RTLD_DEFAULT, name);
        Dl_info from{};
        if (found != nullptr) {
            REQUIRE(dladdr(found, &from) != 0);
            CHECK(from.dli_fbase != mine.dli_fbase);
        }
    }
    // Nor anything else of the test's.
    CHECK(dlsym(RTLD_DEFAULT, "fbe_gpu_start") == nullptr);
}
#endif

TEST_CASE("drawing what runs out of memory says so, and the next frame draws", "[gpu]") {
    // Started, with what every frame needs made.
    const auto drawn_after = [](bool by_camera, const std::function<void(const rig_t&)>& each) {
        for (long which = 0; which < 5000; ++which) {
            const rig_t rig;
            const std::uint64_t target =
                fbe_texture_create_target(16, 16, FBE_TEXTURE_RGBA8, 1, true);
            const std::uint64_t camera = fbe_entity_create(rig.world, 0, FBE_ENTITY_CAMERA);
            REQUIRE(fbe_camera_set_target(camera, target));
            fbe_entity_set_position(rig.body, 0.0F, 0.0F, -3.0F, FBE_SPACE_LOCAL);
            const std::array<std::uint32_t, 3> triangle{ 0, 0, 0 };
            fbe_surface_add_triangles(rig.geometry, 0, triangle.data(), 3);
            each(rig);
            allocated = 0;
            failed = false;
            failing = which;
            const bool done =
                by_camera ? fbe_render_camera(camera, 1.0F) : fbe_render_world(rig.world, 1.0F);
            failing = -1;
            fbe_frame_flip(false);
            const bool had_failed = failed;
            if (had_failed) {
                CHECK(!done);
                CHECK(std::string_view(fbe_error()).contains("out of memory"));
                // Whatever it didn't send, the next frame sends.
                CHECK(fbe_render_world(rig.world, 1.0F));
                fbe_frame_flip(false);
            } else {
                CHECK(done);
            }
            fbe_entity_free(camera);
            fbe_texture_release(target);
            if (!had_failed) {
                return;
            }
        }
        FAIL("a call never got to the end of its allocations");
    };
    for (const bool by_camera : { false, true }) {
        drawn_after(by_camera, [](const rig_t&) {});
        drawn_after(by_camera, [](const rig_t& rig) {
            const std::array<float, 3> moved{ 1.0F, 0.0F, 0.0F };
            REQUIRE(fbe_morph_add_target(rig.geometry, 0, moved.data(), moved.data(), 1) == 0);
            const std::array<float, 1> one{ 1.0F };
            REQUIRE(fbe_morph_set_weights(rig.mesh, one.data(), 1));
        });
    }
    // When a run asks for the layers, they're what said there were none.
    // NOLINTNEXTLINE(clang-diagnostic-deprecated-declarations): only read here.
    const char* wanted = std::getenv("FBE_GPU_VALIDATION");
    if (wanted != nullptr && std::string_view(wanted) == "1") {
        CHECK(fbe_gpu_validating());
    }
    fbe_gpu_stop();
    CHECK(fbe_gpu_validation_errors() == 0);
}

// Growing a string is inside libstdc++, which keeps its own operator new from this test's, so what
// fails here is what the same path takes: more than a string holds.
TEST_CASE("saying what failed, when it can't be kept, says so and doesn't throw") {
    const std::string kept;
    const std::string_view too_long(kept.data(), kept.max_size() + 1);
    CHECK_FALSE(fbe::fail(too_long));
    CHECK(std::string_view(fbe_error()) == "out of memory");
    CHECK_FALSE(fbe::fail("said"));
    CHECK(std::string_view(fbe_error()) == "said");
}

namespace {

/// A buffer's made, written, and let go of with each allocation of it failing in turn, in a frame
/// that's begun, which is all of what could leave one with nothing to free it.
void sweep_buffer(const std::function<bool()>& call) {
    for (long which = 0; which < 2000; ++which) {
        allocated = 0;
        failed = false;
        failing = which;
        bool done = false;
        bool threw = false;
        try {
            done = call();
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        failing = -1;
        if (!failed) {
            CHECK(done);
            return;
        }
        CHECK((threw || !done));
    }
    FAIL("a call never got to the end of its allocations");
}

} // namespace

TEST_CASE("a buffer's made and written out of memory leave nothing to find at the end", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    REQUIRE(fbe_gpu_start());
    const std::array<std::byte, 16> bytes{};
    const auto usage = static_cast<VkBufferUsageFlags>(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::vector<fbe::gpu::buffer_t> made;
    // Room, so what's made isn't lost by this test's keeping it failing.
    made.reserve(2000);
    // In a frame of its own, begun inside the call, as a buffer's first of a frame is.
    fbe_frame_flip(false);
    sweep_buffer([&] {
        const fbe::gpu::buffer_t buffer = fbe::gpu::make_buffer(16, usage);
        made.push_back(buffer);
        return buffer.buffer != VK_NULL_HANDLE;
    });
    fbe_frame_flip(false);
    for (fbe::gpu::buffer_t& buffer : made) {
        fbe::gpu::free_buffer(buffer);
    }
    fbe::gpu::buffer_t written = fbe::gpu::make_buffer(16, usage);
    sweep_buffer([&] { return fbe::gpu::write_buffer(written, 0, bytes); });
    fbe_frame_flip(false);
    sweep_buffer([&] { return !fbe::gpu::read_buffer(written).empty(); });
    // More than a ring holds, so it grows, from one that's there, which goes once the frame's done.
    REQUIRE(fbe::gpu::frame_memory(16, 16).data != nullptr);
    sweep_buffer([&] {
        return fbe::gpu::frame_memory(std::size_t{ 64 } * 1024 * 1024, 16).data != nullptr;
    });
    fbe_frame_flip(false);
    fbe::gpu::free_buffer(written);
    fbe_gpu_stop();
    CHECK(fbe_gpu_validation_errors() == 0);
}
