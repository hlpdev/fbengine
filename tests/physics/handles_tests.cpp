// fbengine_physics' handles: a freed object's handle is never another's, even once its slot is
// used again, a handle of one kind is never another kind's, and no handle is 0.

#include "physics/handles.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>

using fbe::physics::BODY_KIND;
using fbe::physics::pool_t;
using fbe::physics::SHAPE_KIND;

TEST_CASE("a pool gives each object its own handle, never 0") {
    pool_t<int> pool(BODY_KIND);
    const std::uint64_t first = pool.add(std::make_unique<int>(1));
    const std::uint64_t second = pool.add(std::make_unique<int>(2));
    CHECK(first != 0);
    CHECK(second != 0);
    CHECK(first != second);
    CHECK(*pool.get(first) == 1);
    CHECK(*pool.get(second) == 2);
    CHECK(pool.count() == 2);
    CHECK(pool.get(0) == nullptr);
}

TEST_CASE("a handle is its slot, its kind, and its generation") {
    pool_t<int> pool(BODY_KIND);
    const std::uint64_t handle = pool.add(std::make_unique<int>(1));
    CHECK(static_cast<std::uint32_t>(handle) == 0);
    CHECK(static_cast<std::uint8_t>(handle >> 32U) == BODY_KIND);
    CHECK(handle >> 40U == 1);
}

TEST_CASE("a freed handle finds nothing, though its slot is used again") {
    pool_t<int> pool(BODY_KIND);
    const std::uint64_t first = pool.add(std::make_unique<int>(1));
    CHECK(pool.remove(first));
    CHECK_FALSE(pool.remove(first));
    CHECK(pool.get(first) == nullptr);
    const std::uint64_t again = pool.add(std::make_unique<int>(3));
    // The same slot, a new generation.
    CHECK(static_cast<std::uint32_t>(again) == static_cast<std::uint32_t>(first));
    CHECK(again != first);
    CHECK(pool.get(first) == nullptr);
    CHECK(*pool.get(again) == 3);
    CHECK(pool.count() == 1);
}

TEST_CASE("a handle of another kind finds nothing, though its slot and generation match") {
    pool_t<int> bodies(BODY_KIND);
    pool_t<int> shapes(SHAPE_KIND);
    const std::uint64_t body = bodies.add(std::make_unique<int>(1));
    const std::uint64_t shape = shapes.add(std::make_unique<int>(2));
    CHECK(body != shape);
    CHECK(bodies.get(shape) == nullptr);
    CHECK(shapes.get(body) == nullptr);
    CHECK_FALSE(shapes.remove(body));
    CHECK(*bodies.get(body) == 1);
}

TEST_CASE("a handle of a slot past the pool's finds nothing") {
    pool_t<int> pool(BODY_KIND);
    pool.add(std::make_unique<int>(1));
    const std::uint64_t past =
        (std::uint64_t{ 1 } << 40U) | (std::uint64_t{ BODY_KIND } << 32U) | 7U;
    CHECK(pool.get(past) == nullptr);
    CHECK_FALSE(pool.remove(past));
}

TEST_CASE("a slot freed at its last generation is retired, never used again") {
    pool_t<int> pool(BODY_KIND, 3);
    std::uint64_t handle = 0;
    for (std::uint64_t generation = 1; generation <= 3; ++generation) {
        handle = pool.add(std::make_unique<int>(1));
        CHECK(static_cast<std::uint32_t>(handle) == 0);
        CHECK(handle >> 40U == generation);
        CHECK(pool.remove(handle));
    }
    // Slot 0 is retired, so the next object has slot 1.
    const std::uint64_t next = pool.add(std::make_unique<int>(2));
    CHECK(static_cast<std::uint32_t>(next) == 1);
    CHECK(pool.get(handle) == nullptr);
    CHECK(pool.count() == 1);
}
