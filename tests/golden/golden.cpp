// The golden image harness: PNGs through stb, compared within a tolerance; and, updating, written
// once a test that drew them has passed.

#include "golden.hpp"

#include <algorithm>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fbengine.h>
#include <filesystem>
#include <format>
#include <SDL3/SDL_stdinc.h>
#include <stb_image.h>
#include <stb_image_write.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/// Where golden images are kept, in the source; where frames that don't match go, in the build;
/// and where an update's images wait for their test to pass: FBENGINE_GOLDEN_IMAGES,
/// FBENGINE_GOLDEN_FAILURES, and FBENGINE_GOLDEN_UPDATES, which CMake defines.
constexpr std::string_view images = FBENGINE_GOLDEN_IMAGES;
constexpr std::string_view failures = FBENGINE_GOLDEN_FAILURES;
constexpr std::string_view updates = FBENGINE_GOLDEN_UPDATES;

/// How loosely a frame from another GPU is compared: its edges and filtering may differ by a
/// little, as the RTX 4080's don't from lavapipe's, so a wrong frame or a shifted one fails.
constexpr fbe::golden::tolerance_t elsewhere{ .channel = 24, .share = 0.0025 };

/// The images the running test wrote, updating, each where it waits and where it goes once the
/// test passes.
std::vector<std::pair<std::filesystem::path, std::filesystem::path>> written;

/// Moves an update's images into place once their test passes, and leaves them if it doesn't.
class golden_listener_t : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testCaseStarting(const Catch::TestCaseInfo& /*info*/) override {
        written.clear();
    }

    void testCaseEnded(const Catch::TestCaseStats& stats) override {
        const bool passed =
            stats.totals.assertions.allPassed() && stats.totals.testCases.skipped == 0;
        for (const auto& [staged, golden] : written) {
            std::error_code error;
            if (passed) {
                std::filesystem::copy_file(
                    staged, golden, std::filesystem::copy_options::overwrite_existing, error);
            }
            const std::string line = passed && !error
                                         ? std::format("wrote {}\n", golden.string())
                                         : std::format("didn't write {}, as {} failed\n",
                                                       golden.string(), stats.testInfo->name);
            std::fputs(line.c_str(), stderr);
        }
        written.clear();
    }
};

CATCH_REGISTER_LISTENER(golden_listener_t)

/// Where `name`'s golden image is, and where its frame goes when it doesn't match.
std::filesystem::path golden_image(std::string_view name) {
    return std::filesystem::path(images) / std::format("{}.png", name);
}

std::filesystem::path failure(std::string_view name, std::string_view kind = "") {
    return std::filesystem::path(failures) / std::format("{}{}.png", name, kind);
}

bool updating() {
    const char* mode = SDL_getenv("FBENGINE_GOLDEN");
    return mode != nullptr && std::string_view(mode) == "update";
}

bool write_png(const std::filesystem::path& path, const fbe::golden::frame_t& frame) {
    std::filesystem::create_directories(path.parent_path());
    const int stride = static_cast<int>(frame.width) * 4;
    return stbi_write_png(path.string().c_str(), static_cast<int>(frame.width),
                          static_cast<int>(frame.height), 4, frame.rgba.data(), stride) != 0;
}

} // namespace

namespace fbe::golden {

bool on_lavapipe() {
    const std::int32_t device = fbe_gpu_device();
    return fbe_gpu_started() && fbe_gpu_device_kind(device) == FBE_GPU_CPU &&
           std::string_view(fbe_gpu_device_name(device)).starts_with("llvmpipe");
}

void check(std::string_view name, const frame_t& frame, tolerance_t tolerance) {
    REQUIRE(frame.rgba.size() == std::size_t{ frame.width } * frame.height * 4);
    const std::filesystem::path golden = golden_image(name);
    const bool reference = on_lavapipe();
    if (updating()) {
        if (!reference) {
            FAIL("golden images are lavapipe's; this GPU is "
                 << fbe_gpu_device_name(fbe_gpu_device()));
        }
        // It's written once the test passes, so an update never takes a wrong frame.
        const std::filesystem::path staged = std::filesystem::path(updates) / golden.filename();
        REQUIRE(write_png(staged, frame));
        written.emplace_back(staged, golden);
        return;
    }
    const tolerance_t strict = tolerance;
    if (!reference) {
        tolerance.channel = std::max(tolerance.channel, elsewhere.channel);
        tolerance.share = std::max(tolerance.share, elsewhere.share);
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* loaded = stbi_load(golden.string().c_str(), &width, &height, &channels, 4);
    if (loaded == nullptr) {
        write_png(failure(name), frame);
        FAIL("there's no golden image " << golden.string()
                                        << "; tools/golden.py writes it from lavapipe");
    }
    frame_t expected{
        .width = static_cast<std::uint32_t>(width),
        .height = static_cast<std::uint32_t>(height),
    };
    expected.rgba.assign(loaded, loaded + (std::size_t{ expected.width } * expected.height * 4));
    stbi_image_free(loaded);

    const bool same_size = expected.width == frame.width && expected.height == frame.height;
    const difference_t difference =
        same_size ? compare(expected, frame, tolerance.channel) : difference_t{ .share = 1.0 };
    if (difference.share > tolerance.share) {
        write_png(failure(name), frame);
        if (same_size) {
            write_png(failure(name, ".difference"), difference.image);
        }
    }
    INFO(name << ": the frame is " << frame.width << "x" << frame.height << ", the golden image "
              << width << "x" << height);
    REQUIRE(same_size);
    // Off lavapipe, how far it is, strictly and loosely, is shown whether it passes or not, so
    // CI's logs say how close MoltenVK comes.
    if (!reference) {
        const difference_t strictly = compare(expected, frame, strict.channel);
        WARN("not lavapipe, so " << name << " is compared loosely: " << difference.pixels
                                 << " pixels (" << difference.share * 100.0
                                 << "%) differ by more than " << int{ tolerance.channel } << ", "
                                 << strictly.pixels << " (" << strictly.share * 100.0
                                 << "%) by more than " << int{ strict.channel } << ", at most "
                                 << difference.furthest << " away");
    }
    INFO(name << ": " << difference.pixels << " pixels differ by more than "
              << int{ tolerance.channel } << ", " << difference.share * 100.0
              << "% of them, at most " << difference.furthest << " away; see "
              << failure(name).string() << " and its difference");
    CHECK(difference.share <= tolerance.share);
}

difference_t compare(const frame_t& expected, const frame_t& frame, std::uint8_t channel) {
    difference_t difference{ .image = { .width = frame.width, .height = frame.height } };
    const std::size_t count = std::size_t{ frame.width } * frame.height;
    difference.image.rgba.resize(count * 4);
    for (std::size_t pixel = 0; pixel < count; ++pixel) {
        int away = 0;
        for (std::size_t at = pixel * 4; at < (pixel * 4) + 4; ++at) {
            away = std::max(away, std::abs(int{ frame.rgba[at] } - int{ expected.rgba[at] }));
        }
        difference.furthest = std::max(difference.furthest, away);
        const bool differs = away > int{ channel };
        difference.pixels += differs ? 1 : 0;
        for (std::size_t at = pixel * 4; at < (pixel * 4) + 3; ++at) {
            difference.image.rgba[at] =
                differs ? 255 : static_cast<std::uint8_t>(frame.rgba[at] / 4);
        }
        difference.image.rgba[(pixel * 4) + 3] = 255;
    }
    difference.share =
        count == 0 ? 0.0 : static_cast<double>(difference.pixels) / static_cast<double>(count);
    return difference;
}

} // namespace fbe::golden
