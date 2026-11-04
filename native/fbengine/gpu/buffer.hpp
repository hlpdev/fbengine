#pragma once

// Buffers on the GPU, for vertices, indices, uniforms, and storage: written through the frame's
// uploads and freed once the GPU's done with the frame; and a ring a frame in flight of
// host-visible memory, for what's written every frame, as 2D's batches. Shaders read them through
// their addresses, given in push constants.

#include "gpu/vulkan.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace fbe::gpu {

/// A buffer `size` bytes on the GPU, zeroed, read as `usage` says, any of VERTEX_BUFFER,
/// INDEX_BUFFER, UNIFORM_BUFFER, and STORAGE_BUFFER, and by its address; an empty one, with
/// fbe_error's why, if it can't be made.
buffer_t make_buffer(VkDeviceSize size, VkBufferUsageFlags usage);

/// Frees a buffer once the GPU's done with the frame being recorded, and empties it.
void free_buffer(buffer_t& buffer);

/// Writes `bytes` into a buffer at `offset` through a staging buffer in the frame's uploads, so
/// every draw of the frame reads them, those recorded before too; false, with fbe_error's why, if
/// they're not in it.
bool write_buffer(const buffer_t& buffer, VkDeviceSize offset, std::span<const std::byte> bytes);

/// Writes `bytes` into a buffer as `regions` say, whose sources are offsets into `bytes` and whose
/// destinations don't overlap, through one staging buffer and one barrier around them all.
bool write_buffer_regions(const buffer_t& buffer, std::span<const std::byte> bytes,
                          std::span<const VkBufferCopy> regions);

/// How many staging buffers have been made for writes, for tests.
std::uint64_t staging_buffers_made();

/// A buffer's bytes, read back after what the frame's recorded so far; empty, with fbe_error's
/// why, if they can't be.
std::vector<std::byte> read_buffer(const buffer_t& buffer);

/// Space in the frame's ring: its buffer, where in it, its address there, and where the CPU
/// writes it.
struct slice_t {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceAddress address = 0;
    std::byte* data = nullptr;
};

/// `size` bytes of the frame's ring, at an offset `alignment` divides (a power of two), which the
/// CPU writes through `data` before the frame's submitted and the GPU reads in the frame, as
/// vertices, indices, uniforms, or storage: what's written every frame. They're the frame's until
/// the GPU's done with it; each frame in flight has a ring of its own, which grows as a frame
/// needs. An empty slice, with fbe_error's why, if there's no memory for it.
slice_t frame_memory(VkDeviceSize size, VkDeviceSize alignment);

} // namespace fbe::gpu
