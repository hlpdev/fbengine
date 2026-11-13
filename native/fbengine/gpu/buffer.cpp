// Buffers on the GPU, written through staging buffers in the frame's uploads, freed after their
// frame; and each frame in flight's ring of host-visible, coherent memory, given out a slice at a
// time and started again as the frame begins.

#include "gpu/buffer.hpp"

#include "core/engine.hpp"
#include "gpu/image.hpp"
#include "gpu/vulkan.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <format>
#include <span>
#include <vector>

namespace {

using fbe::gpu::flag;

/// The least a ring holds, which it doubles from as a frame needs more.
constexpr VkDeviceSize least_ring = VkDeviceSize{ 1 } << 20U;

/// What every buffer and ring can be read as.
constexpr auto readable = flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) |
                          flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_INDEX_BUFFER_BIT) |
                          flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) |
                          flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

/// The stages and accesses that read a buffer: as vertices, indices, uniforms, or storage, in any
/// shader, which writes to it wait for and are made visible to.
constexpr auto readers =
    flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT) |
    flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT) |
    flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT) |
    flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) |
    flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
constexpr auto reads = flag<VkAccessFlags2>(VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT) |
                       flag<VkAccessFlags2>(VK_ACCESS_2_INDEX_READ_BIT) |
                       flag<VkAccessFlags2>(VK_ACCESS_2_UNIFORM_READ_BIT) |
                       flag<VkAccessFlags2>(VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
constexpr auto transfer = flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COPY_BIT) |
                          flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_CLEAR_BIT);
constexpr auto transfer_write = flag<VkAccessFlags2>(VK_ACCESS_2_TRANSFER_WRITE_BIT);

void buffer_barrier(VkCommandBuffer commands, VkBuffer buffer, VkPipelineStageFlags2 before_stages,
                    VkAccessFlags2 before, VkPipelineStageFlags2 after_stages,
                    VkAccessFlags2 after) {
    const VkBufferMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = before_stages,
        .srcAccessMask = before,
        .dstStageMask = after_stages,
        .dstAccessMask = after,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer,
        .size = VK_WHOLE_SIZE,
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &barrier,
    };
    fbe::gpu::device()->pipeline_barrier(commands, &dependency);
}

VkDeviceAddress address_of(VkBuffer buffer) {
    const VkBufferDeviceAddressInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buffer,
    };
    return vkGetBufferDeviceAddress(fbe::gpu::device()->device, &info);
}

/// A buffer of host-visible, coherent memory the CPU writes or reads through `mapped`, for
/// `usage`; false, with fbe_error's why, if it can't be made.
bool make_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool reading,
                      fbe::gpu::buffer_t& made, std::byte*& mapped) {
    const fbe::gpu::device_t& gpu = *fbe::gpu::device();
    const VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo allocation{
        .flags =
            (reading ? flag<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT)
                     : flag<VmaAllocationCreateFlags>(
                           VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT)) |
            flag<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_MAPPED_BIT),
        .usage = VMA_MEMORY_USAGE_AUTO,
        .requiredFlags = flag<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) |
                         flag<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VmaAllocationInfo found{};
    const VkResult result = fbe::gpu::allocated([&] {
        return vmaCreateBuffer(gpu.allocator, &info, &allocation, &made.buffer, &made.memory,
                               &found);
    });
    if (result != VK_SUCCESS) {
        made = {};
        return fbe::gpu::fail_vulkan("couldn't make a buffer the CPU reaches", result);
    }
    made.size = size;
    made.usage = usage;
    mapped = static_cast<std::byte*>(found.pMappedData);
    if (mapped == nullptr) {
        vmaDestroyBuffer(gpu.allocator, made.buffer, made.memory);
        made = {};
        return fbe::fail("couldn't reach a buffer's memory from the CPU");
    }
    return true;
}

/// How many staging buffers writes have made.
std::uint64_t staging_made = 0;

} // namespace

namespace fbe::gpu {

buffer_t make_buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    const device_t* gpu = device();
    if (gpu == nullptr) {
        fail("the GPU hasn't started");
        return {};
    }
    if (size == 0 || (usage & ~readable) != 0) {
        fail(std::format("a buffer is at least a byte, read as vertices, indices, uniforms, or "
                         "storage, not {} bytes for {:#x}",
                         size, usage));
        return {};
    }
    const VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage | flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_TRANSFER_DST_BIT) |
                 flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_TRANSFER_SRC_BIT) |
                 flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo allocation{ .usage = VMA_MEMORY_USAGE_AUTO };
    buffer_t made{ .size = size, .usage = usage };
    const VkResult result = allocated([&] {
        return vmaCreateBuffer(gpu->allocator, &info, &allocation, &made.buffer, &made.memory,
                               nullptr);
    });
    if (result != VK_SUCCESS) {
        fail_vulkan("couldn't make a buffer", result);
        return {};
    }
    made.address = address_of(made.buffer);
    try {
        VkCommandBuffer commands = upload_commands();
        buffer_barrier(commands, made.buffer, readers, 0, transfer, transfer_write);
        vkCmdFillBuffer(commands, made.buffer, 0, VK_WHOLE_SIZE, 0);
        buffer_barrier(commands, made.buffer, transfer, transfer_write, readers, reads);
    } catch (...) {
        // Out of memory, before any command that names it.
        vmaDestroyBuffer(gpu->allocator, made.buffer, made.memory);
        throw;
    }
    return made;
}

void free_buffer(buffer_t& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE && device() != nullptr) {
        after_use(
            [made = buffer] { vmaDestroyBuffer(device()->allocator, made.buffer, made.memory); });
    }
    buffer = {};
}

bool write_buffer_regions(const buffer_t& buffer, std::span<const std::byte> bytes,
                          std::span<const VkBufferCopy> regions) {
    if (device() == nullptr || buffer.buffer == VK_NULL_HANDLE) {
        return fail("there's no such buffer");
    }
    for (const VkBufferCopy& region : regions) {
        if (region.size == 0 || region.srcOffset > bytes.size() ||
            region.size > bytes.size() - region.srcOffset || region.dstOffset > buffer.size ||
            region.size > buffer.size - region.dstOffset) {
            return fail(std::format("{} bytes at {} aren't in a buffer of {}", region.size,
                                    region.dstOffset, buffer.size));
        }
    }
    if (bytes.empty() || regions.empty()) {
        return fail(std::format("{} bytes in {} regions is nothing to write", bytes.size(),
                                regions.size()));
    }
    buffer_t staging{};
    std::byte* mapped = nullptr;
    if (!make_host_buffer(bytes.size(), flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_TRANSFER_SRC_BIT),
                          false, staging, mapped) ||
        mapped == nullptr) {
        return false;
    }
    ++staging_made;
    std::memcpy(mapped, bytes.data(), bytes.size());
    try {
        after_frame(
            [staging] { vmaDestroyBuffer(device()->allocator, staging.buffer, staging.memory); });
    } catch (...) {
        // Out of memory, before any command that names it.
        vmaDestroyBuffer(device()->allocator, staging.buffer, staging.memory);
        throw;
    }
    count_upload(bytes.size());
    VkCommandBuffer commands = upload_commands();
    buffer_barrier(commands, buffer.buffer, readers, 0, transfer, transfer_write);
    vkCmdCopyBuffer(commands, staging.buffer, buffer.buffer,
                    static_cast<std::uint32_t>(regions.size()), regions.data());
    buffer_barrier(commands, buffer.buffer, transfer, transfer_write, readers, reads);
    return true;
}

bool write_buffer(const buffer_t& buffer, VkDeviceSize offset, std::span<const std::byte> bytes) {
    if (bytes.empty() || offset > buffer.size || bytes.size() > buffer.size - offset) {
        if (device() == nullptr || buffer.buffer == VK_NULL_HANDLE) {
            return fail("there's no such buffer");
        }
        return fail(std::format("{} bytes at {} aren't in a buffer of {}", bytes.size(), offset,
                                buffer.size));
    }
    const std::array regions{ VkBufferCopy{ .dstOffset = offset, .size = bytes.size() } };
    return write_buffer_regions(buffer, bytes, regions);
}

std::uint64_t staging_buffers_made() {
    return staging_made;
}

std::vector<std::byte> read_buffer(const buffer_t& buffer) {
    if (device() == nullptr || buffer.buffer == VK_NULL_HANDLE) {
        fail("there's no such buffer");
        return {};
    }
    if (!finish_frame()) {
        return {};
    }
    buffer_t host{};
    std::byte* mapped = nullptr;
    if (!make_host_buffer(buffer.size, flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_TRANSFER_DST_BIT),
                          true, host, mapped)) {
        return {};
    }
    const bool ran = run_now([&](VkCommandBuffer commands) {
        // After what wrote it, which left it for shaders to read, not for this.
        buffer_barrier(commands, buffer.buffer, readers | transfer, reads | transfer_write,
                       transfer, flag<VkAccessFlags2>(VK_ACCESS_2_TRANSFER_READ_BIT));
        const VkBufferCopy region{ .size = buffer.size };
        vkCmdCopyBuffer(commands, buffer.buffer, host.buffer, 1, &region);
        buffer_barrier(commands, host.buffer, transfer, transfer_write,
                       flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_HOST_BIT),
                       flag<VkAccessFlags2>(VK_ACCESS_2_HOST_READ_BIT));
    });
    std::vector<std::byte> bytes;
    try {
        if (ran) {
            bytes.assign(mapped, mapped + buffer.size);
        }
    } catch (...) {
        vmaDestroyBuffer(device()->allocator, host.buffer, host.memory);
        throw;
    }
    vmaDestroyBuffer(device()->allocator, host.buffer, host.memory);
    return bytes;
}

slice_t frame_memory(VkDeviceSize size, VkDeviceSize alignment) {
    if (device() == nullptr) {
        fail("the GPU hasn't started");
        return {};
    }
    // A lost GPU's frame begins nothing, and its ring grows no more.
    if (lost()) {
        fail_lost();
        return {};
    }
    frame_t& frame = recording_frame();
    const VkDeviceSize start = (frame.ring_used + alignment - 1) & ~(alignment - 1);
    if (frame.ring.buffer == VK_NULL_HANDLE || start + size > frame.ring.size) {
        // A ring that's too small goes once the GPU's done with the frame, as what the frame's
        // given out of it so far is still read.
        const VkDeviceSize grown = std::max({ least_ring, frame.ring.size * 2, size + alignment });
        buffer_t ring{};
        std::byte* mapped = nullptr;
        if (!make_host_buffer(
                grown,
                readable | flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT),
                false, ring, mapped)) {
            return {};
        }
        if (frame.ring.buffer != VK_NULL_HANDLE) {
            try {
                after_frame([old = frame.ring] {
                    vmaDestroyBuffer(device()->allocator, old.buffer, old.memory);
                });
            } catch (...) {
                // Out of memory, so the frame keeps the ring it has.
                vmaDestroyBuffer(device()->allocator, ring.buffer, ring.memory);
                throw;
            }
        }
        ring.address = address_of(ring.buffer);
        frame.ring = ring;
        frame.ring_data = mapped;
        frame.ring_used = 0;
        return frame_memory(size, alignment);
    }
    frame.ring_used = start + size;
    return {
        .buffer = frame.ring.buffer,
        .offset = start,
        .address = frame.ring.address + start,
        .data = frame.ring_data + start,
    };
}

} // namespace fbe::gpu
