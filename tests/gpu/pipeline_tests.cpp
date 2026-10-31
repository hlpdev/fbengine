// Shaders and pipelines, from fbengine's insides: a triangle drawn into an image and compared
// with its golden image, pipelines made once, and the pipeline cache's file.

#include "golden/golden.hpp"
#include "gpu_testing.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fbengine.h>
#include <filesystem>
#include <fstream>
#include <gpu/image.hpp>
#include <gpu/pipeline.hpp>
#include <gpu/vulkan.hpp>
#include <iterator>
#include <string>
#include <test_shaders.hpp>
#include <vector>

namespace {

using fbe::gpu::flag;

constexpr VkFormat rgba = VK_FORMAT_R8G8B8A8_UNORM;

/// Starts the GPU, headless, keeping no pipeline cache unless a test says.
void start(const std::string& cache = "") {
    REQUIRE(fbe_window_set_headless(true));
    fbe_gpu_set_pipeline_cache(cache.c_str());
    fbe::tests::require(fbe_gpu_start());
}

/// A file of this test's in the system's temporary folder, gone as the test starts.
std::filesystem::path scratch(const std::string& name) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "fbengine-tests" / name;
    std::filesystem::remove(path);
    return path;
}

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> chars((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    for (std::size_t i = 0; i < chars.size(); ++i) {
        bytes[i] = static_cast<std::byte>(chars[i]);
    }
    return bytes;
}

/// The triangle shader drawn over a dark grey clear into an image `size` big, read back.
fbe::golden::frame_t draw_triangle(VkExtent2D size) {
    fbe::gpu::image_t image = fbe::gpu::make_color_image(rgba, size);
    REQUIRE(image.image != VK_NULL_HANDLE);
    VkPipeline pipeline = fbe::gpu::pipeline({
        .shader = &fbe::test_shaders::triangle,
        .color = rgba,
    });
    INFO(fbe_error());
    REQUIRE(pipeline != VK_NULL_HANDLE);
    const fbe::gpu::device_t& gpu = *fbe::gpu::device();
    const auto output =
        flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    const auto write = flag<VkAccessFlags2>(VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    REQUIRE(fbe::gpu::run_now([&](VkCommandBuffer commands) {
        fbe::gpu::barrier(commands,
                          fbe::gpu::color_transition(image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                     output, 0, output, write));
        const VkRenderingAttachmentInfo color{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = image.view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = { .color = { .float32 = { 0.1F, 0.1F, 0.1F, 1.0F } } },
        };
        const VkRenderingInfo rendering{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = { .extent = size },
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color,
        };
        gpu.begin_rendering(commands, &rendering);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const VkViewport viewport{
            .width = static_cast<float>(size.width),
            .height = static_cast<float>(size.height),
            .maxDepth = 1.0F,
        };
        vkCmdSetViewport(commands, 0, 1, &viewport);
        const VkRect2D scissor{ .extent = size };
        vkCmdSetScissor(commands, 0, 1, &scissor);
        vkCmdDraw(commands, 3, 1, 0, 0);
        gpu.end_rendering(commands);
    }));
    fbe::golden::frame_t frame{
        .width = size.width,
        .height = size.height,
        .rgba = fbe::gpu::read_pixels(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
    };
    fbe::gpu::destroy_image(image);
    return frame;
}

} // namespace

TEST_CASE("a triangle matches its golden image", "[gpu][golden]") {
    start();
    const fbe::golden::frame_t frame = draw_triangle({ .width = 128, .height = 128 });
    // The clear's grey in a corner, and red near the top corner.
    REQUIRE(frame.rgba.size() == std::size_t{ 128 } * 128 * 4);
    CHECK(frame.rgba[0] == frame.rgba[1]);
    const std::size_t top = ((std::size_t{ 16 } * 128) + 64) * 4;
    CHECK(frame.rgba[top] > 200);
    CHECK(frame.rgba[top + 2] < 50);
    fbe::golden::check("triangle", frame);
    fbe::tests::stop_gpu();
}

TEST_CASE("a pipeline described the same is made once", "[gpu]") {
    start();
    const fbe::gpu::pipeline_desc_t desc{ .shader = &fbe::test_shaders::triangle, .color = rgba };
    const std::int32_t before = fbe_gpu_pipelines();
    const auto started = std::chrono::steady_clock::now();
    VkPipeline first = fbe::gpu::pipeline(desc);
    const std::chrono::duration<double, std::milli> spent =
        std::chrono::steady_clock::now() - started;
    WARN("a pipeline made with no cache: " << spent.count() << " ms on "
                                           << fbe_gpu_device_name(fbe_gpu_device()));
    REQUIRE(first != VK_NULL_HANDLE);
    CHECK(fbe::gpu::pipeline(desc) == first);
    CHECK(fbe_gpu_pipelines() == before + 1);
    fbe::gpu::pipeline_desc_t blended = desc;
    blended.blend = fbe::gpu::blend_t::ALPHA;
    VkPipeline second = fbe::gpu::pipeline(blended);
    CHECK(second != VK_NULL_HANDLE);
    CHECK(second != first);
    CHECK(fbe_gpu_pipelines() == before + 2);
    fbe::tests::stop_gpu();
    CHECK(fbe_gpu_pipelines() == 0);
}

TEST_CASE("a pipeline isn't made before the GPU starts", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    CHECK(fbe::gpu::pipeline({ .shader = &fbe::test_shaders::triangle, .color = rgba }) ==
          VK_NULL_HANDLE);
    CHECK(std::string(fbe_error()) == "the GPU hasn't started");
}

TEST_CASE("a pipeline cache's file unpacks only whole and on its own device and driver", "[gpu]") {
    const VkPhysicalDeviceProperties properties{
        .driverVersion = 7,
        .vendorID = 0x10de,
        .deviceID = 0x2704,
        .pipelineCacheUUID = { 1, 2, 3 },
    };
    const std::vector<std::byte> data{ std::byte{ 9 }, std::byte{ 8 }, std::byte{ 7 } };
    const std::vector<std::byte> packed = fbe::gpu::pack_pipeline_cache(data, properties);
    CHECK(fbe::gpu::unpack_pipeline_cache(packed, properties) == data);

    std::vector<std::byte> changed = packed;
    changed.back() = std::byte{ 6 };
    CHECK(fbe::gpu::unpack_pipeline_cache(changed, properties).empty());
    std::vector<std::byte> cut = packed;
    cut.pop_back();
    CHECK(fbe::gpu::unpack_pipeline_cache(cut, properties).empty());
    CHECK(fbe::gpu::unpack_pipeline_cache({ packed.data(), 10 }, properties).empty());
    CHECK(fbe::gpu::unpack_pipeline_cache({}, properties).empty());

    VkPhysicalDeviceProperties other = properties;
    other.driverVersion = 8;
    CHECK(fbe::gpu::unpack_pipeline_cache(packed, other).empty());
    other = properties;
    other.deviceID = 0x2705;
    CHECK(fbe::gpu::unpack_pipeline_cache(packed, other).empty());
    other = properties;
    other.pipelineCacheUUID[15] = 1;
    CHECK(fbe::gpu::unpack_pipeline_cache(packed, other).empty());
}

TEST_CASE("the pipeline cache is written at the Flip after pipelines are made", "[gpu]") {
    const std::filesystem::path path = scratch("written/pipelines.bin");
    start(path.string());
    CHECK(fbe_gpu_pipeline_cache() == path.string());
    fbe_frame_flip(false);
    // Nothing's made, so nothing's written.
    CHECK(!std::filesystem::exists(path));
    REQUIRE(fbe::gpu::pipeline({ .shader = &fbe::test_shaders::triangle, .color = rgba }) !=
            VK_NULL_HANDLE);
    CHECK(!std::filesystem::exists(path));
    fbe_frame_flip(false);
    REQUIRE(std::filesystem::exists(path));
    const VkPhysicalDeviceProperties& properties = fbe::gpu::device()->properties;
    const std::vector<std::byte> first = read_file(path);
    CHECK(!fbe::gpu::unpack_pipeline_cache(first, properties).empty());
    // Written again only after another's made.
    std::filesystem::remove(path);
    fbe_frame_flip(false);
    CHECK(!std::filesystem::exists(path));
    REQUIRE(fbe::gpu::pipeline({ .shader = &fbe::test_shaders::triangle,
                                 .color = rgba,
                                 .blend = fbe::gpu::blend_t::ADD }) != VK_NULL_HANDLE);
    fbe_frame_flip(false);
    CHECK(!fbe::gpu::unpack_pipeline_cache(read_file(path), properties).empty());
    CHECK(!std::filesystem::exists(std::filesystem::path(path) += ".partial"));
    fbe::tests::stop_gpu();
}

// Stopping writes what's new first, so a program that stops its GPU keeps what it made.
TEST_CASE("the pipelines are freed as the GPU stops, the cache written, and made again after",
          "[gpu]") {
    const std::filesystem::path path = scratch("stopped/pipelines.bin");
    start(path.string());
    const fbe::gpu::pipeline_desc_t desc{ .shader = &fbe::test_shaders::triangle, .color = rgba };
    REQUIRE(fbe::gpu::pipeline(desc) != VK_NULL_HANDLE);
    fbe::tests::stop_gpu();
    CHECK(fbe_gpu_pipelines() == 0);
    CHECK(std::filesystem::exists(path));
    start(path.string());
    CHECK(fbe::gpu::pipeline(desc) != VK_NULL_HANDLE);
    CHECK(fbe_gpu_pipelines() == 1);
    const fbe::golden::frame_t frame = draw_triangle({ .width = 128, .height = 128 });
    fbe::golden::check("triangle", frame);
    fbe::tests::stop_gpu();
}

TEST_CASE("a pipeline cache file that isn't whole is left, and pipelines are still made", "[gpu]") {
    const std::filesystem::path path = scratch("broken/pipelines.bin");
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream file(path, std::ios::binary);
        file << "not a pipeline cache";
    }
    start(path.string());
    CHECK(fbe::gpu::pipeline({ .shader = &fbe::test_shaders::triangle, .color = rgba }) !=
          VK_NULL_HANDLE);
    fbe_frame_flip(false);
    CHECK(
        !fbe::gpu::unpack_pipeline_cache(read_file(path), fbe::gpu::device()->properties).empty());
    fbe::tests::stop_gpu();
}

TEST_CASE("a program's pipeline cache is in the user's cache folder by default", "[gpu]") {
    const std::string path = fbe::gpu::default_pipeline_cache();
    INFO(path);
    CHECK(path.ends_with("/fbengine/fbengine_gpu_tests/pipelines.bin"));
    CHECK(std::string(fbe_gpu_pipeline_cache()) == path);
    fbe_gpu_set_pipeline_cache("");
    CHECK(std::string(fbe_gpu_pipeline_cache()).empty());
}
