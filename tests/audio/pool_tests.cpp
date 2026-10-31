// fbengine_audio's handle pool, whose old handles must find nothing, even once their slots hold
// something new, and whose handles another kind's pool refuses.

#include "audio/handles.hpp"
#include "audio/pool.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>

namespace {

using fbe::audio::kind_t;
using pool_t = fbe::audio::pool_t<int>;

} // namespace

TEST_CASE("a pool finds what it was given by handle, and none is 0") {
    pool_t pool(kind_t::SOUND);
    const std::uint64_t first = pool.add(std::make_unique<int>(1));
    const std::uint64_t second = pool.add(std::make_unique<int>(2));
    CHECK(first != 0);
    CHECK(second != first);
    CHECK(*pool.find(first) == 1);
    CHECK(*pool.find(second) == 2);
    CHECK(pool.find(0) == nullptr);
    CHECK(pool.count() == 2);
}

// A 32-bit slot, a 24-bit generation from 1, and the kind in the top 8 bits, as
// native/common/handles.hpp lays them out.
TEST_CASE("a handle is its slot, generation, and kind") {
    pool_t pool(kind_t::BUS);
    pool.add(std::make_unique<int>(1));
    const std::uint64_t handle = pool.add(std::make_unique<int>(2));
    CHECK(handle == ((std::uint64_t{ 66 } << 56U) | (std::uint64_t{ 1 } << 32U) | 1U));
    CHECK(fbe::audio::handle_kind(handle) == kind_t::BUS);
    CHECK(fbe::audio::handle_generation(handle) == 1);
    CHECK(fbe::audio::handle_slot(handle) == 1);
}

// A Bus's handle given for a Sound, in the same slot with the same generation, is refused, never
// taken for the Sound there.
TEST_CASE("a pool refuses another kind's handles") {
    pool_t sounds(kind_t::SOUND);
    pool_t buses(kind_t::BUS);
    pool_t channels(kind_t::CHANNEL);
    pool_t emitters(kind_t::EMITTER);
    const std::uint64_t sound = sounds.add(std::make_unique<int>(1));
    const std::uint64_t bus = buses.add(std::make_unique<int>(2));
    const std::uint64_t channel = channels.add(std::make_unique<int>(3));
    const std::uint64_t emitter = emitters.add(std::make_unique<int>(4));
    CHECK(sounds.find(bus) == nullptr);
    CHECK(sounds.find(channel) == nullptr);
    CHECK(buses.find(sound) == nullptr);
    CHECK(channels.find(emitter) == nullptr);
    CHECK(emitters.find(sound) == nullptr);
    CHECK(sounds.remove(bus) == nullptr);
    CHECK(*buses.find(bus) == 2);
    CHECK(*sounds.find(sound) == 1);
}

TEST_CASE("a removed handle finds nothing, though its slot is used again") {
    pool_t pool(kind_t::CHANNEL);
    const std::uint64_t old = pool.add(std::make_unique<int>(1));
    CHECK(*pool.remove(old) == 1);
    CHECK(pool.remove(old) == nullptr);
    const std::uint64_t reused = pool.add(std::make_unique<int>(2));
    CHECK(fbe::audio::handle_slot(reused) == fbe::audio::handle_slot(old));
    CHECK(reused != old);
    CHECK(pool.find(old) == nullptr);
    CHECK(*pool.find(reused) == 2);
}

TEST_CASE("clearing a pool leaves none of its handles finding anything") {
    pool_t pool(kind_t::EMITTER);
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
    pool_t pool(kind_t::SOUND);
    const std::uint64_t odd = pool.add(std::make_unique<int>(1));
    const std::uint64_t even = pool.add(std::make_unique<int>(2));
    pool.remove_if([](int item) { return item % 2 == 0; });
    CHECK(pool.find(odd) != nullptr);
    CHECK(pool.find(even) == nullptr);
    CHECK(pool.count() == 1);
}

// A slot is used for every generation from 1 to the most 24 bits hold, then retired, so neither 0
// nor a handle from its first use ever comes back.
TEST_CASE("a slot whose generation would wrap is retired") {
    pool_t pool(kind_t::SOUND);
    const std::uint64_t first = pool.add(std::make_unique<int>(0));
    std::uint64_t last = first;
    pool.remove(first);
    for (std::uint32_t generation = 2; generation <= fbe::audio::most_generation; ++generation) {
        last = pool.add(std::make_unique<int>(1));
        REQUIRE(fbe::audio::handle_slot(last) == 0);
        pool.remove(last);
    }
    CHECK(fbe::audio::handle_generation(last) == fbe::audio::most_generation);
    CHECK(pool.slots() == 1);
    const std::uint64_t next = pool.add(std::make_unique<int>(2));
    CHECK(fbe::audio::handle_slot(next) == 1);
    CHECK(pool.slots() == 2);
    CHECK(pool.find(first) == nullptr);
    CHECK(pool.find(last) == nullptr);
    CHECK(*pool.find(next) == 2);
}
