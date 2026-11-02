// The golden image harness's comparing, with no GPU.

#include "golden/golden.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

namespace {

/// A frame `width` by 2 of one grey.
fbe::golden::frame_t grey(std::uint32_t width, std::uint8_t level) {
    fbe::golden::frame_t frame{ .width = width, .height = 2 };
    frame.rgba.assign(std::size_t{ width } * 2 * 4, level);
    return frame;
}

} // namespace

TEST_CASE("the same frame doesn't differ", "[golden]") {
    const fbe::golden::difference_t same = fbe::golden::compare(grey(4, 100), grey(4, 100), 0);
    CHECK(same.pixels == 0);
    CHECK(same.share == 0.0);
    CHECK(same.furthest == 0);
}

TEST_CASE("a pixel differs when a channel is further than the tolerance", "[golden]") {
    fbe::golden::frame_t frame = grey(4, 100);
    // The third pixel's green, 3 away.
    frame.rgba[9] = 103;
    CHECK(fbe::golden::compare(grey(4, 100), frame, 3).pixels == 0);
    const fbe::golden::difference_t off = fbe::golden::compare(grey(4, 100), frame, 2);
    CHECK(off.pixels == 1);
    CHECK(off.share == 1.0 / 8.0);
    CHECK(off.furthest == 3);
    // Where it differs is white in the difference's image, and the rest a dim copy.
    CHECK(off.image.rgba[8] == 255);
    CHECK(off.image.rgba[0] == 25);
    CHECK(off.image.rgba[3] == 255);
}

TEST_CASE("alpha counts as a channel", "[golden]") {
    fbe::golden::frame_t frame = grey(1, 100);
    frame.rgba[3] = 0;
    CHECK(fbe::golden::compare(grey(1, 100), frame, 99).pixels == 1);
}
