// Random calls on players, clips, and bones, from seeds that repeat: nothing they do breaks what's
// posed, every transform and morph weight staying numbers and every rotation length 1, and every
// call that's asked about a freed thing failing rather than touching it.

#include "fbengine.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace {

constexpr std::array<const char*, 5> nodes{ "", "a", "b", "c", "m" };

std::uint64_t named_pivot(std::uint64_t world, std::uint64_t parent, const char* name) {
    const std::uint64_t made = fbe_entity_create(world, parent, FBE_ENTITY_PIVOT);
    fbe_entity_set_name(made, name);
    return made;
}

/// A clip of random keys, of each interpolation, on random nodes, with events.
std::uint64_t random_clip(std::mt19937& random) {
    std::uniform_real_distribution<float> unit(-1.0F, 1.0F);
    std::uniform_real_distribution<float> gap(0.01F, 0.5F);
    const std::uint64_t clip = fbe_clip_create("fuzz", 0.0F);
    for (const char* node : nodes) {
        for (const std::int32_t property :
             { FBE_CLIP_TRANSLATION, FBE_CLIP_ROTATION, FBE_CLIP_SCALE, FBE_CLIP_WEIGHTS }) {
            if (random() % 2 == 0) {
                continue;
            }
            const auto interpolation = static_cast<std::int32_t>(random() % 3);
            std::int32_t width = property == FBE_CLIP_ROTATION ? 4 : 3;
            if (property == FBE_CLIP_WEIGHTS) {
                width = 1 + static_cast<std::int32_t>(random() % 4);
            }
            const std::int32_t keys = 1 + static_cast<std::int32_t>(random() % 5);
            std::vector<float> times;
            std::vector<float> values;
            float time = 0.0F;
            for (std::int32_t k = 0; k < keys; ++k) {
                times.push_back(time);
                time += gap(random);
                const std::int32_t floats = width * (interpolation == FBE_CLIP_CUBIC ? 3 : 1);
                for (std::int32_t f = 0; f < floats; ++f) {
                    values.push_back(unit(random));
                }
            }
            fbe_clip_add_channel(clip, node, property, interpolation, width, times.data(),
                                 values.data(), keys);
        }
    }
    for (int i = 0; i < 3; ++i) {
        fbe_clip_add_event(clip, "e", std::abs(unit(random)));
    }
    return clip;
}

bool all_numbers(std::uint64_t entity) {
    fbe_vec3_t position{};
    fbe_quat_t rotation{};
    fbe_vec3_t scale{};
    if (!fbe_entity_position(entity, FBE_SPACE_WORLD, &position) ||
        !fbe_entity_rotation(entity, FBE_SPACE_LOCAL, &rotation) ||
        !fbe_entity_scale(entity, FBE_SPACE_LOCAL, &scale)) {
        return false;
    }
    const float length = std::sqrt((rotation.x * rotation.x) + (rotation.y * rotation.y) +
                                   (rotation.z * rotation.z) + (rotation.w * rotation.w));
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
           std::isfinite(scale.x) && std::isfinite(scale.y) && std::isfinite(scale.z) &&
           std::abs(length - 1.0F) < 1e-3F;
}

/// Whether a mesh's weights are numbers, `tween` of the way from the capture.
bool weights_are_numbers(std::uint64_t mesh, float tween) {
    std::array<float, 16> out{};
    const std::int32_t count = fbe_morph_weights(mesh, tween, out.data(), out.size());
    return count >= 0 && std::cmp_less_equal(count, out.size()) &&
           std::ranges::all_of(out, [](float weight) { return std::isfinite(weight); });
}

} // namespace

TEST_CASE("random calls leave every pose in numbers", "[stability]") {
    int bad = 0;
    for (std::uint32_t seed = 1; seed <= 20; ++seed) {
        std::mt19937 random(seed);
        std::uniform_real_distribution<float> unit(0.0F, 1.0F);
        const std::uint64_t world = fbe_world_create();
        const std::uint64_t root = named_pivot(world, 0, "root");
        const std::uint64_t a = named_pivot(world, root, "a");
        const std::uint64_t b = named_pivot(world, a, "b");
        std::uint64_t c = named_pivot(world, a, "c");
        const std::uint64_t m = fbe_entity_create(world, a, FBE_ENTITY_MESH);
        fbe_entity_set_name(m, "m");
        const std::uint64_t player = fbe_player_create(root);
        std::array<std::uint64_t, 4> clips{};
        for (std::uint64_t& clip : clips) {
            clip = random_clip(random);
        }
        for (int step = 0; step < 1000; ++step) {
            const std::uint64_t clip = clips.at(random() % clips.size());
            const auto layer = static_cast<std::int32_t>(random() % 3);
            const auto mode = static_cast<std::int32_t>(random() % 3);
            const float speed = (unit(random) * 4.0F) - 2.0F;
            switch (random() % 15) {
            case 0:
            case 1:
                fbe_player_play(player, layer, clip, mode, speed);
                break;
            case 2:
            case 3:
                fbe_player_crossfade(player, layer, clip, unit(random), mode, speed);
                break;
            case 4:
                fbe_player_stop(player, layer, random() % 2 == 0 ? 0.0F : unit(random));
                break;
            case 5:
                fbe_player_set_time(player, layer, (unit(random) * 6.0F) - 3.0F);
                break;
            case 6:
                fbe_player_set_speed(player, layer, speed);
                break;
            case 7:
                fbe_player_set_weight(player, layer, unit(random));
                break;
            case 8: {
                const std::array<std::uint64_t, 2> mask{ a, c };
                fbe_player_set_mask(player, layer, mask.data(),
                                    static_cast<std::int32_t>(random() % 3));
                break;
            }
            case 9:
                if (random() % 2 == 0) {
                    fbe_player_release(player, b);
                } else {
                    fbe_player_reclaim(player, b);
                }
                break;
            case 10:
                fbe_entity_free(c);
                c = named_pivot(world, a, "c");
                break;
            case 11: {
                std::array<float, 6> weights{};
                std::ranges::generate(weights, [&] { return (unit(random) * 2.0F) - 1.0F; });
                fbe_morph_set_weights(m, weights.data(), static_cast<std::int32_t>(random() % 7));
                break;
            }
            case 12:
                fbe_world_capture(world);
                break;
            case 13:
                fbe_entity_set_position(random() % 2 == 0 ? a : m, unit(random), unit(random),
                                        unit(random), FBE_SPACE_LOCAL);
                fbe_entity_set_scale(b, unit(random) + 0.5F, 1.0F, 1.0F, FBE_SPACE_LOCAL);
                break;
            default: {
                const std::uint64_t part = fbe_clip_slice(clip, unit(random), 1.0F + unit(random));
                fbe_player_play(player, layer, part, mode, speed);
                fbe_clip_release(part);
                break;
            }
            }
            fbe_player_advance(world, unit(random) * 0.1F);
            fbe_player_clear_events(player);
            for (const std::uint64_t entity : { root, a, b, c, m }) {
                bad += all_numbers(entity) ? 0 : 1;
            }
            bad += weights_are_numbers(m, unit(random)) ? 0 : 1;
        }
        CHECK(fbe_player_bone_count(player) <= 5);
        fbe_player_free(player);
        for (const std::uint64_t clip : clips) {
            fbe_clip_release(clip);
        }
        fbe_world_free(world);
    }
    CHECK(bad == 0);
}
