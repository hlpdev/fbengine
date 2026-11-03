// A program that opens a video, plays it, and ends at once, as a game that panics in a cutscene
// does, with its thread decoding; physics/ends.cmake runs it again and again.

#include "fbengine_video.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

int main() try {
    std::ifstream file(FBE_VIDEO_MEDIA "/colors.mpg", std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
    const std::uint64_t video =
        fbe_video_open(bytes.data(), static_cast<std::int64_t>(bytes.size()));
    if (video == 0) {
        return 2;
    }
    fbe_video_play(video);
    fbe_video_update(video, 0.04);
    // As a panic's program exits, with 101.
    std::exit(101);
} catch (...) {
    return 3;
}
