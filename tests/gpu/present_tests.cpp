// The GPU through fbengine's C API: starting it, stopping it, and Flip presenting a headless
// window, which draws into an image of its own, as a shown one's swapchain's. A swapchain needs a
// window on screen, which no test opens. Each test ends by stopping the GPU, which has to leave
// nothing behind.

#include "gpu_testing.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <fbengine.h>
#include <format>
#include <gpu/image.hpp>
#include <gpu/vulkan.hpp>
#include <SDL3/SDL.h>
#include <string>
#include <string_view>
#include <texture/texture.hpp>

using fbe::gpu::flag;
using fbe::tests::require;
using fbe::tests::set_variable;
using fbe::tests::stop_gpu;

namespace {

/// A headless window, open while it's in scope, with the GPU started.
struct window_t {
    window_t(std::int32_t width, std::int32_t height) {
        REQUIRE(fbe_window_set_headless(true));
        require(fbe_window_open(width, height, FBE_WINDOW_WINDOWED));
        require(fbe_gpu_start());
    }
    window_t(const window_t&) = delete;
    window_t& operator=(const window_t&) = delete;
    window_t(window_t&&) = delete;
    window_t& operator=(window_t&&) = delete;
    ~window_t() {
        fbe_window_close();
    }
};

bool swapchain_is_windows_size() {
    return fbe_gpu_swapchain_width() == fbe_window_pixel_width() &&
           fbe_gpu_swapchain_height() == fbe_window_pixel_height();
}

/// What SDL logged while a test listened.
std::string logged;

void listen(void* /*user*/, int /*category*/, SDL_LogPriority /*priority*/, const char* message) {
    logged += message;
    logged += '\n';
}

} // namespace

TEST_CASE("the GPU starts on a device with what fbengine needs, with no window", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    REQUIRE(!fbe_gpu_started());
    REQUIRE(fbe_gpu_device_count() >= 1);
    REQUIRE(fbe_gpu_device() == -1);
    require(fbe_gpu_start());
    REQUIRE(fbe_gpu_started());
    const std::int32_t device = fbe_gpu_device();
    REQUIRE(device >= 0);
    CHECK(std::string_view(fbe_gpu_device_lacks(device)).empty());
    WARN("GPU: " << fbe_gpu_device_name(device)
                 << (fbe_gpu_validating() ? ", validated" : ", not validated"));
    // Its device is set before it starts.
    CHECK(!fbe_gpu_set_device(-1));
    stop_gpu();
}

// CI sets FBE_GPU_VALIDATION where it installs the layers, so a run that should validate does.
TEST_CASE("the GPU validates when FBE_GPU_VALIDATION is 1", "[gpu]") {
    const char* setting = SDL_getenv("FBE_GPU_VALIDATION");
    REQUIRE(fbe_window_set_headless(true));
    require(fbe_gpu_start());
    if (setting != nullptr && std::string_view(setting) == "1") {
        CHECK(fbe_gpu_validating());
        // And its synchronization, which the frame graph's barriers are checked by.
        CHECK(fbe::gpu::validating_synchronization());
    }
    stop_gpu();
}

// Synchronization's checked, so a barrier left out is an error: two clears of one image, the second
// with no barrier after the first.
TEST_CASE("validation catches a barrier left out", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    require(fbe_gpu_start());
    if (!fbe::gpu::validating_synchronization()) {
        fbe_gpu_stop();
        SKIP("the validation layers here don't check synchronization");
    }
    const std::uint64_t texture = fbe_texture_create(4, 4, FBE_TEXTURE_RGBA8, false);
    REQUIRE(texture != 0);
    VkImage image = fbe::texture::find(texture)->image;
    const std::int32_t before = fbe_gpu_validation_errors();
    const auto everything = flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    const VkImageSubresourceRange whole{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto moved = [&](VkImageLayout from, VkImageLayout to) {
        return VkImageMemoryBarrier2{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = everything,
            .srcAccessMask = flag<VkAccessFlags2>(VK_ACCESS_2_MEMORY_WRITE_BIT),
            .dstStageMask = everything,
            .dstAccessMask = flag<VkAccessFlags2>(VK_ACCESS_2_MEMORY_READ_BIT) |
                             flag<VkAccessFlags2>(VK_ACCESS_2_MEMORY_WRITE_BIT),
            .oldLayout = from,
            .newLayout = to,
            .image = image,
            .subresourceRange = whole,
        };
    };
    require(fbe::gpu::run_now([&](VkCommandBuffer commands) {
        const VkClearColorValue red{ .float32 = { 1.0F, 0.0F, 0.0F, 1.0F } };
        fbe::gpu::barrier(commands, moved(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
        vkCmdClearColorImage(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1,
                             &whole);
        vkCmdClearColorImage(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1,
                             &whole);
        fbe::gpu::barrier(commands, moved(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    }));
    CHECK(fbe_gpu_validation_errors() > before);
    fbe_texture_release(texture);
    // Stopped as it is, as stop_gpu requires no errors.
    fbe_gpu_stop();
}

// As the loader's filter says, where it takes it, which CI's for an administrator doesn't, and as a
// fault says where it doesn't: either way there's no device, and no GPU.
TEST_CASE("with no driver, the GPU doesn't start, and says why each time", "[gpu]") {
    set_variable("VK_LOADER_DRIVERS_DISABLE", "*");
    fbe::gpu::inject(fbe::gpu::fault_t::NO_DRIVER);
    REQUIRE(fbe_window_set_headless(true));
    CHECK(!fbe_gpu_start());
    const std::string why = fbe_error();
    CHECK(!why.empty());
    CHECK(fbe_gpu_device_count() <= 0);
    CHECK(!fbe_gpu_start());
    CHECK(fbe_error() == why);
    CHECK(!fbe_gpu_started());
    CHECK(fbe_gpu_presented() == 0);
    // A headless window opens without it, and Flip has nothing to present.
    require(fbe_window_open(320, 200, FBE_WINDOW_WINDOWED));
    fbe_frame_flip(false);
    CHECK(fbe_gpu_presented() == 0);
    fbe_window_close();
    fbe_gpu_stop();
}

TEST_CASE("a start that fails is final, and frees what it made", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    // The device is made, and then its frames fail.
    fbe::gpu::inject(fbe::gpu::fault_t::DEVICE);
    CHECK(!fbe_gpu_start());
    const std::string why = fbe_error();
    CHECK(why.starts_with("couldn't make the GPU's frames"));
    CHECK(!fbe_gpu_start());
    CHECK(fbe_error() == why);
    CHECK(!fbe_gpu_started());
    CHECK(fbe::gpu::device() == nullptr);
    stop_gpu();
}

TEST_CASE("a device that isn't there can't be picked", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    const std::int32_t count = fbe_gpu_device_count();
    CHECK(!fbe_gpu_set_device(count));
    CHECK(std::string(fbe_error()) == "there's no GPU " + std::to_string(count));
    CHECK(!fbe_gpu_set_device(-2));
    CHECK(std::string_view(fbe_gpu_device_name(count)).empty());
    CHECK(fbe_gpu_set_device(0));
    CHECK(fbe_gpu_device() == 0);
    fbe_gpu_stop();
}

TEST_CASE("a headless window opened before the GPU starts gets its image as it starts", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    require(fbe_window_open(320, 200, FBE_WINDOW_WINDOWED));
    // A headless window doesn't start the GPU.
    CHECK(!fbe_gpu_started());
    CHECK(fbe_gpu_swapchain_width() == 0);
    require(fbe_gpu_start());
    CHECK(fbe_gpu_swapchain_headless());
    CHECK(swapchain_is_windows_size());
    fbe_window_close();
    CHECK(fbe_gpu_swapchain_width() == 0);
    stop_gpu();
}

// SDL's offscreen video can't show a window, so it's headless to the GPU, whose start it doesn't
// need, and which draws it into an image of its own.
TEST_CASE("a window on SDL's offscreen video that isn't headless gets an image of its own",
          "[gpu]") {
    set_variable("SDL_VIDEO_DRIVER", "offscreen");
    REQUIRE(!fbe_window_headless());
    require(fbe_window_open(320, 200, FBE_WINDOW_WINDOWED));
    CHECK(!fbe_gpu_started());
    require(fbe_gpu_start());
    CHECK(fbe_gpu_swapchain_headless());
    const std::int64_t before = fbe_gpu_presented();
    fbe_frame_flip(false);
    CHECK(fbe_gpu_presented() == before + 1);
    fbe_window_close();
    stop_gpu();
}

TEST_CASE("Flip presents with vsync and without", "[gpu]") {
    const window_t window(320, 200);
    const std::int64_t before = fbe_gpu_presented();
    fbe_frame_flip(true);
    CHECK(fbe_gpu_presented() == before + 1);
    CHECK(fbe_gpu_swapchain_vsync());
    fbe_frame_flip(false);
    fbe_frame_flip(false);
    CHECK(fbe_gpu_presented() == before + 3);
    CHECK(!fbe_gpu_swapchain_vsync());
    fbe_frame_flip(true);
    CHECK(fbe_gpu_swapchain_vsync());
    stop_gpu();
}

TEST_CASE("the window's images follow its size and mode", "[gpu]") {
    const window_t window(320, 200);
    fbe_frame_flip(false);
    CHECK(swapchain_is_windows_size());
    REQUIRE(fbe_window_open(640, 480, FBE_WINDOW_WINDOWED));
    const std::int64_t before = fbe_gpu_presented();
    fbe_frame_flip(false);
    CHECK(fbe_gpu_presented() == before + 1);
    CHECK(swapchain_is_windows_size());
    REQUIRE(fbe_window_set_mode(FBE_WINDOW_BORDERLESS));
    fbe_frame_flip(false);
    CHECK(swapchain_is_windows_size());
    stop_gpu();
}

// Flip presents before it reads the OS's events, so the frame after the one that read them is the
// first they change.
TEST_CASE("a minimized window isn't presented", "[gpu]") {
    const window_t window(320, 200);
    REQUIRE(fbe_window_simulate(FBE_WINDOW_SIMULATE_MINIMIZE));
    fbe_frame_flip(false);
    const std::int64_t before = fbe_gpu_presented();
    fbe_frame_flip(false);
    fbe_frame_flip(false);
    CHECK(fbe_gpu_presented() == before);
    REQUIRE(fbe_window_simulate(FBE_WINDOW_SIMULATE_RESTORE));
    fbe_frame_flip(false);
    fbe_frame_flip(false);
    CHECK(fbe_gpu_presented() == before + 1);
    stop_gpu();
}

TEST_CASE("a window opened again is presented again", "[gpu]") {
    {
        const window_t window(320, 200);
        fbe_frame_flip(false);
    }
    const std::int64_t before = fbe_gpu_presented();
    fbe_frame_flip(false);
    CHECK(fbe_gpu_presented() == before);
    const window_t again(200, 100);
    fbe_frame_flip(false);
    CHECK(fbe_gpu_presented() == before + 1);
    CHECK(swapchain_is_windows_size());
    stop_gpu();
}

TEST_CASE("a stopped GPU frees everything, keeps the window, and starts again", "[gpu]") {
    const window_t window(320, 200);
    fbe_frame_flip(false);
    stop_gpu();
    CHECK(fbe_gpu_swapchain_width() == 0);
    CHECK(fbe_window_is_open());
    // Flip has nothing to present, and stopping again is nothing.
    const std::int64_t before = fbe_gpu_presented();
    fbe_frame_flip(false);
    CHECK(fbe_gpu_presented() == before);
    fbe_gpu_stop();
    require(fbe_gpu_start());
    CHECK(swapchain_is_windows_size());
    fbe_frame_flip(false);
    CHECK(fbe_gpu_presented() == before + 1);
    stop_gpu();
}

// The fence and acquire semaphore a failed submit leaves are made again, or the next Flip with
// that frame would wait forever.
TEST_CASE("a frame whose submit fails doesn't stop the next ones", "[gpu]") {
    const window_t window(320, 200);
    fbe_frame_flip(false);
    const std::int64_t before = fbe_gpu_presented();
    fbe::gpu::inject(fbe::gpu::fault_t::SUBMIT);
    fbe_frame_flip(false);
    CHECK(fbe_gpu_presented() == before);
    for (int i = 0; i < 4; ++i) {
        fbe_frame_flip(false);
    }
    CHECK(fbe_gpu_presented() == before + 4);
    stop_gpu();
}

TEST_CASE("a window's image whose view fails is gone, and the next Flip makes it again", "[gpu]") {
    const window_t window(320, 200);
    fbe_frame_flip(false);
    REQUIRE(fbe_window_open(640, 480, FBE_WINDOW_WINDOWED));
    const std::int64_t before = fbe_gpu_presented();
    fbe::gpu::inject(fbe::gpu::fault_t::VIEW);
    fbe_frame_flip(false);
    CHECK(fbe_gpu_presented() == before);
    CHECK(fbe_gpu_swapchain_width() == 0);
    fbe_frame_flip(false);
    CHECK(fbe_gpu_presented() == before + 1);
    CHECK(swapchain_is_windows_size());
    stop_gpu();
}

// SDL's offscreen video logs, as it loads Vulkan, when the driver has no headless surfaces, which
// Windows' never have and fbengine never asks it for.
TEST_CASE("starting the GPU logs nothing of headless surfaces", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    SDL_LogOutputFunction before = nullptr;
    void* user = nullptr;
    SDL_GetLogOutputFunction(&before, &user);
    SDL_SetLogOutputFunction(listen, nullptr);
    logged.clear();
    const bool started = fbe_gpu_start();
    SDL_SetLogOutputFunction(before, user);
    require(started);
    CHECK(!logged.contains("headless"));
    stop_gpu();
}

// Not a check, a measure: what Flip costs the CPU for an empty frame, which the budget's 2 ms
// covers, shown on every run.
TEST_CASE("an empty frame's Flip, timed", "[gpu]") {
    const window_t window(1280, 720);
    constexpr int frames = 200;
    for (int i = 0; i < 10; ++i) {
        fbe_frame_flip(false);
    }
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        fbe_frame_flip(false);
    }
    const std::chrono::duration<double, std::milli> spent =
        std::chrono::steady_clock::now() - start;
    WARN("an empty 1280x720 frame: " << spent.count() / frames << " ms a Flip on "
                                     << fbe_gpu_device_name(fbe_gpu_device()));
    stop_gpu();
}

// The commands work now runs on are made the first time; a try that fails leaves nothing made,
// and the next makes them.
TEST_CASE("work run now whose commands can't be made leaks nothing, and runs next time", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    require(fbe_gpu_start());
    fbe::gpu::inject(fbe::gpu::fault_t::WORK_NOW);
    CHECK(!fbe::gpu::run_now([](VkCommandBuffer /*commands*/) {}));
    CHECK(std::string(fbe_error()).starts_with("couldn't make the GPU's commands for work now"));
    CHECK(fbe::gpu::run_now([](VkCommandBuffer /*commands*/) {}));
    stop_gpu();
}

// The GPU's lost as a submit is: nothing runs from then on, Flip presents nothing, what needs the
// GPU says it was lost, and stopping and starting it makes it again.
TEST_CASE("a lost GPU runs nothing, says so, and starts again", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    fbe_gpu_set_pipeline_cache("");
    require(fbe_window_open(64, 32, FBE_WINDOW_WINDOWED));
    require(fbe_gpu_start());
    const std::uint64_t texture = fbe_texture_create(4, 4, FBE_TEXTURE_RGBA8, false);
    REQUIRE(texture != 0);
    require(fbe_texture_clear(texture, 1.0F, 0.0F, 0.0F, 1.0F));
    const std::int64_t presented = fbe_gpu_presented();
    fbe::gpu::inject(fbe::gpu::fault_t::LOST);
    fbe_frame_flip(false);
    CHECK(fbe_gpu_lost());
    CHECK(fbe_gpu_presented() == presented);
    const std::string lost = "the GPU was lost, as a driver's crash or reset loses it; stopping it "
                             "and starting it again makes it again";
    CHECK(fbe_texture_create(4, 4, FBE_TEXTURE_RGBA8, false) == 0);
    CHECK(std::string(fbe_error()) == lost);
    // A pass is still added, and dropped, as nothing runs.
    require(fbe_texture_clear(texture, 0.0F, 1.0F, 0.0F, 1.0F));
    std::array<std::uint8_t, 64> pixels{};
    CHECK(!fbe_texture_read(texture, 0, pixels.data(), 64));
    CHECK(std::string(fbe_error()) == lost);
    fbe_frame_flip(false);
    CHECK(fbe_gpu_presented() == presented);
    stop_gpu();
    CHECK(!fbe_gpu_lost());
    require(fbe_gpu_start());
    const std::int64_t again = fbe_gpu_presented();
    fbe_frame_flip(false);
    CHECK(fbe_gpu_presented() == again + 1);
    stop_gpu();
}

// The GPU's lost as work run now is, as a read's is: the read fails saying so, nothing runs from
// then on, and stopping and starting it makes it again.
TEST_CASE("a GPU lost running work now says so, and starts again", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    require(fbe_gpu_start());
    const std::uint64_t texture = fbe_texture_create(4, 4, FBE_TEXTURE_RGBA8, false);
    REQUIRE(texture != 0);
    std::array<std::uint8_t, 64> pixels{};
    fbe::gpu::inject(fbe::gpu::fault_t::LOST_NOW);
    CHECK(!fbe_texture_read(texture, 0, pixels.data(), 64));
    CHECK(fbe_gpu_lost());
    CHECK(std::string(fbe_error()).starts_with("the GPU was lost"));
    const std::int64_t presented = fbe_gpu_presented();
    fbe_frame_flip(false);
    CHECK(fbe_gpu_presented() == presented);
    CHECK(fbe_texture_create(4, 4, FBE_TEXTURE_RGBA8, false) == 0);
    fbe_texture_release(texture);
    stop_gpu();
    CHECK(!fbe_gpu_lost());
    require(fbe_gpu_start());
    const std::uint64_t again = fbe_texture_create(4, 4, FBE_TEXTURE_RGBA8, false);
    CHECK(again != 0);
    CHECK(fbe_texture_read(again, 0, pixels.data(), 64));
    fbe_texture_release(again);
    stop_gpu();
}

// Every device lacks dynamic rendering, as a GPU from before Vulkan 1.3 can: the GPU doesn't start,
// and says which lacks what, as each device does.
TEST_CASE("a GPU that lacks what fbengine needs says what it lacks", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    fbe::gpu::inject(fbe::gpu::fault_t::LACKS);
    CHECK(!fbe_gpu_start());
    const std::string why = fbe_error();
    INFO(why);
    CHECK(why.starts_with("no GPU has what fbengine needs: "));
    CHECK(why.contains(fbe_gpu_device_name(0)));
    CHECK(why.ends_with("dynamic rendering"));
    CHECK(std::string_view(fbe_gpu_device_lacks(0)).ends_with("dynamic rendering"));
    CHECK(!fbe_gpu_started());
    stop_gpu();
}

// The device asked for lacks it, once it's listed again as the GPU starts.
TEST_CASE("a GPU asked for that lacks what fbengine needs says what it lacks", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    require(fbe_gpu_set_device(0));
    fbe::gpu::inject(fbe::gpu::fault_t::LACKS);
    CHECK(!fbe_gpu_start());
    const std::string why = fbe_error();
    INFO(why);
    CHECK(why == std::format("the GPU {} lacks what fbengine needs: {}", fbe_gpu_device_name(0),
                             fbe_gpu_device_lacks(0)));
    CHECK(why.ends_with("dynamic rendering"));
    require(fbe_gpu_set_device(-1));
    stop_gpu();
}

TEST_CASE("the GPU's memory budget and usage are its own memory's", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    CHECK(fbe_gpu_memory_budget() == -1);
    CHECK(fbe_gpu_memory_usage() == -1);
    require(fbe_gpu_start());
    const std::int64_t budget = fbe_gpu_memory_budget();
    CHECK(budget > 0);
    // The white texture and the frames' commands are made as it starts.
    CHECK(fbe_gpu_memory_usage() > 0);
    CHECK(fbe_gpu_memory_usage() <= budget);
    fbe_frame_flip(false);
    CHECK(fbe_gpu_memory_budget() > 0);
    stop_gpu();
}

// The device's lost as a Flip with nothing recorded asks whether the frames are done.
TEST_CASE("a lost GPU is caught on a Flip with nothing to run", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    require(fbe_gpu_start());
    fbe_frame_flip(false);
    CHECK(!fbe_gpu_lost());
    fbe::gpu::inject(fbe::gpu::fault_t::LOST);
    fbe_frame_flip(false);
    CHECK(fbe_gpu_lost());
    CHECK(std::string(fbe_error()).starts_with("the GPU was lost"));
    stop_gpu();
}
