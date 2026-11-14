// fbengine_native's frame, from a program's first flip: ctest runs each test in a process of its
// own.

#include "fbengine.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <limits>
#include <thread>

// SDL's clock starts at the first flip, so the first's time can be 0, and the second is still the
// second: it waits for the limit, and has a delta.
TEST_CASE("TheSecondFlipHasADelta") {
    REQUIRE(fbe_frame_set_limit(1000));
    fbe_frame_flip(false);
    CHECK(fbe_frame_count() == 1);
    CHECK(fbe_frame_delta() == 0.0);
    fbe_frame_flip(false);
    CHECK(fbe_frame_count() == 2);
    CHECK(fbe_frame_delta() >= 0.001);
}

// A fixed delta is every frame's, whatever the clock says, the first's too, and once it's taken
// away, the clock's again; one under 0 or not finite is refused, leaving what was.
TEST_CASE("AFixedDeltaIsEveryFramesUntilItsTakenAway", "[stability]") {
    const double fixed = 1.0 / 32.0;
    REQUIRE(fbe_frame_set_fixed_delta(fixed));
    CHECK(fbe_frame_fixed_delta() == fixed);
    fbe_frame_flip(false);
    CHECK(fbe_frame_delta() == fixed);
    for (int frame = 0; frame < 5; ++frame) {
        // Longer than the fixed delta, then far shorter: neither shows.
        std::this_thread::sleep_for(std::chrono::milliseconds(frame % 2 == 0 ? 40 : 0));
        fbe_frame_flip(false);
        CHECK(fbe_frame_delta() == fixed);
    }
    CHECK_FALSE(fbe_frame_set_fixed_delta(-1.0));
    CHECK_FALSE(fbe_frame_set_fixed_delta(std::numeric_limits<double>::quiet_NaN()));
    CHECK_FALSE(fbe_frame_set_fixed_delta(std::numeric_limits<double>::infinity()));
    CHECK(fbe_frame_fixed_delta() == fixed);
    REQUIRE(fbe_frame_set_fixed_delta(0.0));
    CHECK(fbe_frame_fixed_delta() == 0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    fbe_frame_flip(false);
    CHECK(fbe_frame_delta() >= 0.0);
    CHECK(fbe_frame_delta() != fixed);
}
