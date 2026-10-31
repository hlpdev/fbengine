// Buffers on the GPU and the frame's ring: written through the uploads, read back, drawn from by
// their addresses, and freed after their frame. Each test ends by stopping the GPU, which has to
// leave nothing behind.

#include "gpu_testing.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fbengine.h>
#include <gpu/buffer.hpp>
#include <gpu/pipeline.hpp>
#include <gpu/vulkan.hpp>
#include <span>
#include <string>
#include <test_shaders.hpp>
#include <texture/texture.hpp>
#include <vector>

using fbe::tests::require;
using fbe::tests::stop_gpu;

namespace {

using fbe::gpu::flag;

void start() {
    REQUIRE(fbe_window_set_headless(true));
    fbe_gpu_set_pipeline_cache("");
    require(fbe_gpu_start());
}

constexpr auto storage = flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
constexpr auto indices = flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

template <typename Value>
std::span<const std::byte> bytes_of(const Value& value) {
    return std::as_bytes(std::span(&value, 1));
}

/// What the points shader's push constants are.
struct points_t {
    VkDeviceAddress corners;
    std::uint64_t padding;
    std::array<float, 4> color;
};

/// The corners of a triangle at `x` across, in clip space.
std::array<float, 6> triangle_at(float x) {
    return { x, -0.5F, x - 0.4F, 0.5F, x + 0.4F, 0.5F };
}

std::array<std::uint8_t, 4> pixel(const std::vector<std::uint8_t>& rgba, std::uint32_t width,
                                  std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{ y } * width) + x) * 4;
    return { rgba[at], rgba[at + 1], rgba[at + 2], rgba[at + 3] };
}

} // namespace

TEST_CASE("a buffer is made zeroed, written, and read back", "[gpu]") {
    start();
    fbe::gpu::buffer_t buffer = fbe::gpu::make_buffer(64, storage);
    REQUIRE(buffer.buffer != VK_NULL_HANDLE);
    CHECK(buffer.address != 0);
    CHECK(fbe::gpu::read_buffer(buffer) == std::vector<std::byte>(64, std::byte{ 0 }));
    const std::array<std::uint32_t, 3> values{ 7, 8, 9 };
    require(fbe::gpu::write_buffer(buffer, 8, std::as_bytes(std::span(values))));
    const std::vector<std::byte> read = fbe::gpu::read_buffer(buffer);
    REQUIRE(read.size() == 64);
    std::array<std::uint32_t, 3> back{};
    std::memcpy(back.data(), read.data() + 8, sizeof(back));
    CHECK(back == values);
    CHECK(read[7] == std::byte{ 0 });
    CHECK(read[20] == std::byte{ 0 });
    fbe::gpu::free_buffer(buffer);
    CHECK(buffer.buffer == VK_NULL_HANDLE);
    stop_gpu();
}

TEST_CASE("a buffer refuses what isn't in it, and sizes and uses it can't have", "[gpu]") {
    start();
    fbe::gpu::buffer_t buffer = fbe::gpu::make_buffer(16, storage);
    const std::array<std::byte, 8> eight{};
    CHECK(!fbe::gpu::write_buffer(buffer, 12, eight));
    CHECK(std::string(fbe_error()) == "8 bytes at 12 aren't in a buffer of 16");
    CHECK(!fbe::gpu::write_buffer(buffer, 17, std::span(eight).first(1)));
    CHECK(fbe::gpu::make_buffer(0, storage).buffer == VK_NULL_HANDLE);
    CHECK(fbe::gpu::make_buffer(16, flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
              .buffer == VK_NULL_HANDLE);
    fbe::gpu::free_buffer(buffer);
    stop_gpu();
}

// A buffer written and freed in one frame is freed once the GPU's done with it, so its copy runs.
TEST_CASE("a freed buffer lives until its frame's done", "[gpu]") {
    start();
    fbe::gpu::buffer_t buffer = fbe::gpu::make_buffer(4096, storage);
    const std::vector<std::byte> data(4096, std::byte{ 3 });
    require(fbe::gpu::write_buffer(buffer, 0, data));
    fbe::gpu::free_buffer(buffer);
    fbe_frame_flip(false);
    fbe_frame_flip(false);
    fbe_frame_flip(false);
    stop_gpu();
}

// A draw reads its corners through addresses: a buffer's, written through the uploads; the
// ring's, written by the CPU in the frame; and an index buffer's indices, bound.
TEST_CASE("draws read their corners through buffers' addresses and indices", "[gpu]") {
    start();
    const std::uint64_t target =
        fbe_texture_create_target(48, 16, FBE_TEXTURE_RGBA8_LINEAR, 1, false);
    REQUIRE(target != 0);
    fbe::gpu::buffer_t corners = fbe::gpu::make_buffer(sizeof(float) * 6, storage);
    require(fbe::gpu::write_buffer(corners, 0, bytes_of(triangle_at(-0.66F))));
    const fbe::gpu::slice_t slice = fbe::gpu::frame_memory(sizeof(float) * 6, 16);
    REQUIRE(slice.data != nullptr);
    const std::array<float, 6> middle = triangle_at(0.0F);
    std::memcpy(slice.data, middle.data(), sizeof(middle));
    fbe::gpu::buffer_t order = fbe::gpu::make_buffer(sizeof(std::uint32_t) * 3, indices);
    require(fbe::gpu::write_buffer(order, 0, bytes_of(std::array<std::uint32_t, 3>{ 2, 1, 0 })));
    fbe::gpu::buffer_t right = fbe::gpu::make_buffer(sizeof(float) * 6, storage);
    require(fbe::gpu::write_buffer(right, 0, bytes_of(triangle_at(0.66F))));

    VkPipeline pipeline = fbe::gpu::pipeline({
        .shader = &fbe::test_shaders::points,
        .color = VK_FORMAT_R8G8B8A8_UNORM,
    });
    REQUIRE(pipeline != VK_NULL_HANDLE);
    VkCommandBuffer commands =
        fbe::texture::begin_pass(target, { .color = true, .rgba = { 0.0F, 0.0F, 0.0F, 1.0F } });
    REQUIRE(commands != VK_NULL_HANDLE);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const auto push = [commands](VkDeviceAddress address, std::array<float, 4> color) {
        const points_t constants{ .corners = address, .color = color };
        vkCmdPushConstants(commands, fbe::gpu::pipeline_layout(),
                           flag<VkShaderStageFlags>(VK_SHADER_STAGE_ALL), 0, sizeof(constants),
                           &constants);
    };
    push(corners.address, { 1.0F, 0.0F, 0.0F, 1.0F });
    vkCmdDraw(commands, 3, 1, 0, 0);
    push(slice.address, { 0.0F, 1.0F, 0.0F, 1.0F });
    vkCmdDraw(commands, 3, 1, 0, 0);
    push(right.address, { 0.0F, 0.0F, 1.0F, 1.0F });
    vkCmdBindIndexBuffer(commands, order.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(commands, 3, 1, 0, 0, 0);
    fbe::texture::end_pass(target);

    std::vector<std::uint8_t> rgba(std::size_t{ 48 } * 16 * 4);
    require(fbe_texture_read(target, 0, rgba.data(), static_cast<std::int64_t>(rgba.size())));
    CHECK(pixel(rgba, 48, 8, 9) == std::array<std::uint8_t, 4>{ 255, 0, 0, 255 });
    CHECK(pixel(rgba, 48, 24, 9) == std::array<std::uint8_t, 4>{ 0, 255, 0, 255 });
    CHECK(pixel(rgba, 48, 40, 9) == std::array<std::uint8_t, 4>{ 0, 0, 255, 255 });
    CHECK(pixel(rgba, 48, 24, 1) == std::array<std::uint8_t, 4>{ 0, 0, 0, 255 });
    fbe::gpu::free_buffer(corners);
    fbe::gpu::free_buffer(order);
    fbe::gpu::free_buffer(right);
    stop_gpu();
}

TEST_CASE("the frame's ring gives aligned space, grows, and starts again each frame", "[gpu]") {
    start();
    const fbe::gpu::slice_t first = fbe::gpu::frame_memory(100, 16);
    const fbe::gpu::slice_t second = fbe::gpu::frame_memory(100, 256);
    REQUIRE(first.data != nullptr);
    CHECK(first.offset == 0);
    CHECK(second.buffer == first.buffer);
    CHECK(second.offset == 256);
    CHECK(second.address == first.address + 256);
    // More than it holds: a bigger ring, the old one freed after the frame.
    const fbe::gpu::slice_t big = fbe::gpu::frame_memory(VkDeviceSize{ 3 } << 20U, 16);
    REQUIRE(big.data != nullptr);
    CHECK(big.buffer != first.buffer);
    CHECK(big.offset == 0);
    std::memset(big.data, 1, std::size_t{ 3 } << 20U);
    // The next frame in flight has its own; the one after this one starts it again.
    fbe_frame_flip(false);
    const fbe::gpu::slice_t next = fbe::gpu::frame_memory(64, 16);
    CHECK(next.buffer != big.buffer);
    fbe_frame_flip(false);
    const fbe::gpu::slice_t again = fbe::gpu::frame_memory(64, 16);
    CHECK(again.buffer == big.buffer);
    CHECK(again.offset == 0);
    stop_gpu();
}
