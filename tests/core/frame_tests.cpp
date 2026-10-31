// fbengine_native's frame, from a program's first flip: ctest runs each test in a process of its
// own.

#include "fbengine.h"

#include <catch2/catch_test_macros.hpp>

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
