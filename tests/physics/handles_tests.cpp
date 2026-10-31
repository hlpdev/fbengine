// fbengine_physics' handles: a freed object's handle is never another's, even once its slot is
// used again, and no handle is 0.

#include "physics/handles.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>

using fbe::physics::pool_t;

TEST_CASE("a pool gives each object its own handle, never 0") {
    pool_t<int> pool;
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

TEST_CASE("a freed handle finds nothing, though its slot is used again") {
    pool_t<int> pool;
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

TEST_CASE("a handle of a slot past the pool's finds nothing") {
    pool_t<int> pool;
    pool.add(std::make_unique<int>(1));
    CHECK(pool.get((std::uint64_t{ 1 } << 32U) | 7U) == nullptr);
    CHECK_FALSE(pool.remove((std::uint64_t{ 1 } << 32U) | 7U));
}
