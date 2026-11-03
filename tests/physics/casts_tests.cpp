// Casts at the edges a mesh's triangles share, on tilted grids of many sizes: CastRayAll's one hit
// a surface, of each body, however its hits come from Jolt's broad phase, and a small sphere's
// sweep, which doesn't pass between them, as a ray can.

#include "fbengine_physics.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

// xorshift64, the same each run.
class random_t {
public:
    explicit random_t(std::uint64_t seed) : m_state(seed) {}

    std::uint64_t operator()() {
        m_state ^= m_state << 13U;
        m_state ^= m_state >> 7U;
        m_state ^= m_state << 17U;
        return m_state;
    }

    // From -1 to 1.
    float uniform() {
        return (static_cast<float>((*this)() % 2001U) / 1000.0F) - 1.0F;
    }

private:
    std::uint64_t m_state;
};

struct v3_t {
    float x;
    float y;
    float z;
};

v3_t add(v3_t a, v3_t b) {
    return { .x = a.x + b.x, .y = a.y + b.y, .z = a.z + b.z };
}

v3_t mul(v3_t a, float s) {
    return { .x = a.x * s, .y = a.y * s, .z = a.z * s };
}

v3_t normal(v3_t a) {
    return mul(a, 1.0F / std::sqrt((a.x * a.x) + (a.y * a.y) + (a.z * a.z)));
}

v3_t cross(v3_t a, v3_t b) {
    return {
        .x = (a.y * b.z) - (a.z * b.y),
        .y = (a.z * b.x) - (a.x * b.z),
        .z = (a.x * b.y) - (a.y * b.x),
    };
}

// A square grid of `count` by `count` cells of `cell` meters, about `center`, along `u` and `v`.
std::uint64_t grid(v3_t center, v3_t u, v3_t v, int count, float cell) {
    std::vector<fbe_shape_vec3_t> vertices;
    std::vector<std::uint32_t> indices;
    const int half = count / 2;
    for (int j = 0; j <= count; ++j) {
        for (int i = 0; i <= count; ++i) {
            const v3_t at = add(center, add(mul(u, static_cast<float>(i - half) * cell),
                                            mul(v, static_cast<float>(j - half) * cell)));
            vertices.push_back({ .x = at.x, .y = at.y, .z = at.z });
        }
    }
    const auto across = static_cast<std::uint32_t>(count + 1);
    for (std::uint32_t j = 0; std::cmp_less(j, count); ++j) {
        for (std::uint32_t i = 0; std::cmp_less(i, count); ++i) {
            const std::uint32_t a = (j * across) + i;
            indices.insert(indices.end(),
                           { a, a + across + 1, a + across, a, a + 1, a + across + 1 });
        }
    }
    return fbe_shape_mesh(vertices.data(), static_cast<std::int32_t>(vertices.size()),
                          indices.data(), static_cast<std::int32_t>(indices.size()));
}

// How many rays give more than a hit a body, of `bodies` of a grid of `cell` meters, on rays
// `length` long from `away` meters off it, aimed at its corners, edges, and diagonals.
int too_many(int bodies, float cell, float length, float away) {
    random_t random(0x2545F4914F6CDD1DULL);
    const auto any = [&] {
        const float x = random.uniform();
        const float y = random.uniform();
        const float z = random.uniform();
        return v3_t{ .x = x, .y = y, .z = z };
    };
    const fbe_body_quat_t unturned{ .x = 0.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F };
    const fbe_body_vec3_t origin{ .x = 0.0F, .y = 0.0F, .z = 0.0F };
    std::array<fbe_physics_hit_t, 64> hits{};
    int wrong = 0;
    for (int trial = 0; trial < 5; ++trial) {
        const std::uint64_t world = fbe_physics_new(16);
        const v3_t u = normal(any());
        const v3_t n = normal(cross(u, normal(any())));
        const v3_t v = cross(n, u);
        const v3_t center = mul(any(), 50.0F);
        const std::uint64_t mesh = grid(center, u, v, 6, cell);
        for (int b = 0; b < bodies; ++b) {
            fbe_body_add(world, mesh, FBE_BODY_STATIC, &origin, &unturned);
        }
        fbe_physics_optimize(world);
        for (int ray = 0; ray < 200; ++ray) {
            const auto i = static_cast<float>(static_cast<int>(random() % 5) - 2);
            const auto j = static_cast<float>(static_cast<int>(random() % 5) - 2);
            const float along = static_cast<float>(random() % 1000) / 1000.0F;
            const std::uint64_t how = random() % 3;
            const float first = how == 0 ? i : i + along;
            const float second = how == 2 ? j + along : j;
            const v3_t at = add(center, add(mul(u, first * cell), mul(v, second * cell)));
            const v3_t back = normal(add(n, mul(any(), 0.3F)));
            const v3_t from = add(at, mul(back, away));
            const fbe_physics_vec3_t origin_of{ .x = from.x, .y = from.y, .z = from.z };
            const fbe_physics_vec3_t direction{ .x = -back.x, .y = -back.y, .z = -back.z };
            const int count = fbe_physics_cast_ray_all(world, &origin_of, &direction, length,
                                                       0xFFFFFFFFU, 0, false, hits.data(), 64);
            if (count > bodies) {
                ++wrong;
            }
        }
        fbe_physics_free(world);
        fbe_shape_free(mesh);
    }
    return wrong;
}

// How many of rays aimed at where a tilted grid's triangles meet, from `away` meters off it, a
// sphere of `radius` swept along them misses.
struct misses_t {
    int rays = 0;
    int missed = 0;
};

misses_t misses(float cell, float away, float radius) {
    random_t random(0x9E3779B97F4A7C15ULL);
    const auto any = [&] {
        const float x = random.uniform();
        const float y = random.uniform();
        const float z = random.uniform();
        return v3_t{ .x = x, .y = y, .z = z };
    };
    const fbe_body_quat_t unturned{ .x = 0.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F };
    const fbe_physics_quat_t none{ .x = 0.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F };
    const fbe_body_vec3_t origin{ .x = 0.0F, .y = 0.0F, .z = 0.0F };
    const std::uint64_t sphere = fbe_shape_sphere(radius);
    misses_t found;
    for (int trial = 0; trial < 10; ++trial) {
        const std::uint64_t world = fbe_physics_new(16);
        const v3_t u = normal(any());
        const v3_t n = normal(cross(u, normal(any())));
        const v3_t v = cross(n, u);
        const v3_t center = mul(any(), 50.0F);
        const std::uint64_t mesh = grid(center, u, v, 6, cell);
        fbe_body_add(world, mesh, FBE_BODY_STATIC, &origin, &unturned);
        for (int ray = 0; ray < 200; ++ray) {
            const auto i = static_cast<float>(static_cast<int>(random() % 5) - 2);
            const auto j = static_cast<float>(static_cast<int>(random() % 5) - 2);
            const float along = static_cast<float>(random() % 1000) / 1000.0F;
            const std::uint64_t how = random() % 3;
            const float first = how == 0 ? i : i + along;
            const float second = how == 2 ? j + along : j;
            const v3_t at = add(center, add(mul(u, first * cell), mul(v, second * cell)));
            const v3_t back = normal(add(n, mul(any(), 0.3F)));
            const v3_t from = add(at, mul(back, away));
            const fbe_physics_vec3_t start{ .x = from.x, .y = from.y, .z = from.z };
            const float length = away * 2.0F;
            fbe_physics_hit_t hit{};
            const fbe_physics_vec3_t motion{
                .x = -back.x * length,
                .y = -back.y * length,
                .z = -back.z * length,
            };
            ++found.rays;
            if (!fbe_physics_cast_shape(world, sphere, &start, &none, &motion, 0xFFFFFFFFU, 0,
                                        &hit)) {
                ++found.missed;
            }
        }
        fbe_physics_free(world);
        fbe_shape_free(mesh);
    }
    fbe_shape_free(sphere);
    return found;
}

} // namespace

// A ray aimed where triangles meet can pass between them, as Jolt's test of one isn't watertight;
// what mustn't, as a ground check at a seam, sweeps a sphere of a centimeter, which CastRay's docs
// say.
TEST_CASE("a small sphere's sweep doesn't pass between a mesh's triangles") {
    fbe_physics_set_threads(0);
    for (const float cell : { 2.0F, 20.0F }) {
        for (const float away : { 0.1F, 1.0F, 10.0F, 100.0F, 500.0F }) {
            const misses_t found = misses(cell, away, 0.01F);
            CHECK(found.rays == 2000);
            CHECK(found.missed == 0);
        }
    }
}

TEST_CASE("castrayall gives a hit a body where a ray crosses a mesh's shared edges") {
    fbe_physics_set_threads(0);
    for (const int bodies : { 1, 2 }) {
        for (const float cell : { 2.0F, 20.0F }) {
            for (const float length : { 5.0F, 50.0F, 1000.0F }) {
                for (const float away : { 0.01F, 1.0F, 4.0F }) {
                    CHECK(too_many(bodies, cell, length, away) == 0);
                }
            }
        }
    }
}
