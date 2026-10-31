#pragma once

// The golden image harness: a frame the GPU drew, read back, compared with the PNG checked in
// beside this file within a tolerance. Golden images are lavapipe's, whose frames are the same on
// every machine; on another GPU a frame is compared more loosely, and never written. With
// FBENGINE_GOLDEN=update, a check writes its image instead (tools/golden.py runs the tests so), and
// a changed image is reviewed as code is. A frame that doesn't match is written to
// golden-failures/ in the build, with an image of where it differs.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace fbe::golden {

/// A frame read back: RGBA, 8 bits a channel, rows top first, packed.
struct frame_t {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

/// How far a frame can be from its golden image: a pixel differs when a channel of it is more
/// than `channel` away, and at most `share` of the pixels may differ.
struct tolerance_t {
    std::uint8_t channel = 2;
    double share = 0.0;
};

/// Where one frame differs from another of its size: how many pixels have a channel more than a
/// tolerance's `channel` away, what share of them that is, the furthest any channel is, and an
/// image of them, white on a dim copy of the frame.
struct difference_t {
    std::size_t pixels = 0;
    double share = 0.0;
    int furthest = 0;
    frame_t image;
};

/// Where `frame` differs from `expected`, which is its size.
difference_t compare(const frame_t& expected, const frame_t& frame, std::uint8_t channel);

/// Checks `frame` against golden/<name>.png, failing the test if it doesn't match.
void check(std::string_view name, const frame_t& frame, tolerance_t tolerance = {});

/// Whether the GPU the frames come from is lavapipe, whose frames golden images are.
bool on_lavapipe();

} // namespace fbe::golden
