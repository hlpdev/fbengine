// fbengine_audio's handle pool, whose old handles must find nothing, even once their slots hold
// something new.

#include "audio/pool.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>

namespace {

using pool_t = fbe::audio::pool_t<int>;

} // namespace

TEST_CASE("a pool finds what it was given by handle, and none is 0") {
    pool_t pool;
    const std::uint64_t first = pool.add(std::make_unique<int>(1));
    const std::uint64_t second = pool.add(std::make_unique<int>(2));
    CHECK(first != 0);
    CHECK(second != first);
    CHECK(*pool.find(first) == 1);
    CHECK(*pool.find(second) == 2);
    CHECK(pool.find(0) == nullptr);
    CHECK(pool.count() == 2);
}

TEST_CASE("a removed handle finds nothing, though its slot is used again") {
    pool_t pool;
    const std::uint64_t old = pool.add(std::make_unique<int>(1));
    CHECK(*pool.remove(old) == 1);
    CHECK(pool.remove(old) == nullptr);
    const std::uint64_t reused = pool.add(std::make_unique<int>(2));
    CHECK(static_cast<std::uint32_t>(reused) == static_cast<std::uint32_t>(old));
    CHECK(reused != old);
    CHECK(pool.find(old) == nullptr);
    CHECK(*pool.find(reused) == 2);
}

TEST_CASE("clearing a pool leaves none of its handles finding anything") {
    pool_t pool;
    const std::uint64_t one = pool.add(std::make_unique<int>(1));
    const std::uint64_t two = pool.add(std::make_unique<int>(2));
    pool.clear();
    CHECK(pool.count() == 0);
    pool.add(std::make_unique<int>(3));
    pool.add(std::make_unique<int>(4));
    CHECK(pool.find(one) == nullptr);
    CHECK(pool.find(two) == nullptr);
}

TEST_CASE("remove_if takes out what it says") {
    pool_t pool;
    const std::uint64_t odd = pool.add(std::make_unique<int>(1));
    const std::uint64_t even = pool.add(std::make_unique<int>(2));
    pool.remove_if([](int item) { return item % 2 == 0; });
    CHECK(pool.find(odd) != nullptr);
    CHECK(pool.find(even) == nullptr);
    CHECK(pool.count() == 1);
}

// A slot's generation goes round to 1, never 0, so a handle is never 0.
TEST_CASE("a slot used over and over never gives handle 0") {
    pool_t pool;
    std::uint64_t handle = 0;
    for (int i = 0; i < 1000; ++i) {
        handle = pool.add(std::make_unique<int>(i));
        CHECK(handle != 0);
        pool.remove(handle);
    }
}
