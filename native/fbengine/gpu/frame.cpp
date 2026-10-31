// The frame's commands: begun the first time something records after a Flip, the frame graph
// recorded into them as they're submitted, at the Flip, uploads first, and what each frame in
// flight leaves to free once the GPU's done with it. A submit that fails for want of memory keeps
// the frame's uploads, and what they copy from, for the next submit to run first, so a texture
// written then still gets its pixels.

#include "core/engine.hpp"
#include "gpu/graph.hpp"
#include "gpu/vulkan.hpp"

#include <array>
#include <functional>
#include <utility>
#include <vector>

namespace {

using fbe::gpu::flag;

/// A failed submit's uploads, carried to the next: the frame's pool they're in, which the frame
/// gave up for a new one, and what they wait for to be freed, as their staging buffers.
struct carried_t {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer uploads = VK_NULL_HANDLE;
    std::vector<std::function<void()>> garbage;
};

std::vector<carried_t> carried;

/// Frees what a frame left, which the GPU is done with.
void collect(fbe::gpu::frame_t& frame) {
    // A free can leave more for later, which goes on the list again, after these.
    const std::vector<std::function<void()>> garbage = std::move(frame.garbage);
    frame.garbage.clear();
    for (const std::function<void()>& free : garbage) {
        free();
    }
}

/// Begins the frame's commands, if they aren't: waits for the GPU to be done with the frame in
/// flight that last had them, frees what it left, and begins both its command buffers.
fbe::gpu::frame_t& begin() {
    fbe::gpu::device_t& gpu = *fbe::gpu::device();
    fbe::gpu::frame_t& frame = gpu.frames[gpu.frame];
    if (frame.begun) {
        return frame;
    }
    vkWaitForFences(gpu.device, 1, &frame.done, VK_TRUE, UINT64_MAX);
    collect(frame);
    frame.ring_used = 0;
    vkResetCommandPool(gpu.device, frame.pool, 0);
    const VkCommandBufferBeginInfo info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = flag<VkCommandBufferUsageFlags>(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT),
    };
    vkBeginCommandBuffer(frame.uploads, &info);
    vkBeginCommandBuffer(frame.commands, &info);
    frame.begun = true;
    return frame;
}

/// Frees what a failed submit's uploads were carrying, and their pool, now.
void drop(const fbe::gpu::device_t& gpu, const carried_t& carry) {
    for (const std::function<void()>& free : carry.garbage) {
        free();
    }
    vkDestroyCommandPool(gpu.device, carry.pool, nullptr);
}

/// Ends and submits the frame's commands with its fence, after any uploads a failed submit
/// carried; false, with fbe_error's why, if the submit failed, which makes the fence and acquire
/// semaphore again and, unless the device was lost, carries the frame's uploads to the next.
bool submit(const fbe::gpu::device_t& gpu, fbe::gpu::frame_t& frame, VkSemaphore wait,
            VkSemaphore signal) {
    vkEndCommandBuffer(frame.uploads);
    vkEndCommandBuffer(frame.commands);
    frame.begun = false;
    const auto output =
        flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    const VkSemaphoreSubmitInfo wait_for{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = wait,
        .stageMask = output,
    };
    const VkSemaphoreSubmitInfo signaled{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = signal,
        .stageMask = output,
    };
    std::vector<VkCommandBufferSubmitInfo> commands;
    commands.reserve(carried.size() + 2);
    for (const carried_t& carry : carried) {
        commands.push_back({
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = carry.uploads,
        });
    }
    for (VkCommandBuffer buffer : { frame.uploads, frame.commands }) {
        commands.push_back({
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = buffer,
        });
    }
    const VkSubmitInfo2 info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = wait != VK_NULL_HANDLE ? 1U : 0U,
        .pWaitSemaphoreInfos = &wait_for,
        .commandBufferInfoCount = static_cast<std::uint32_t>(commands.size()),
        .pCommandBufferInfos = commands.data(),
        .signalSemaphoreInfoCount = signal != VK_NULL_HANDLE ? 1U : 0U,
        .pSignalSemaphoreInfos = &signaled,
    };
    vkResetFences(gpu.device, 1, &frame.done);
    const VkResult result = fbe::gpu::faulted(fbe::gpu::fault_t::SUBMIT)
                                ? VK_ERROR_OUT_OF_HOST_MEMORY
                                : gpu.queue_submit(gpu.queue, 1, &info, frame.done);
    if (result != VK_SUCCESS) {
        // Nothing will signal the fence or wait on the acquire, so they're made again, once the
        // GPU's done with everything before.
        vkDeviceWaitIdle(gpu.device);
        // The frame's uploads, and their staging, go to the next submit, in the pool they're in,
        // and the frame gets a new one; on a lost device, or with no memory for a new pool,
        // they're freed with everything carried before.
        VkCommandPool pool = frame.pool;
        VkCommandBuffer uploads = frame.uploads;
        if (result != VK_ERROR_DEVICE_LOST && fbe::gpu::make_frame_commands(gpu, frame)) {
            carried.push_back(
                { .pool = pool, .uploads = uploads, .garbage = std::move(frame.garbage) });
            frame.garbage.clear();
        } else {
            collect(frame);
            for (const carried_t& carry : carried) {
                drop(gpu, carry);
            }
            carried.clear();
        }
        fbe::gpu::remake_frame(gpu, frame);
        return fbe::gpu::fail_vulkan("the GPU couldn't run the frame", result);
    }
    // What was carried is freed with the frame, once the GPU's done with it.
    for (carried_t& carry : carried) {
        for (std::function<void()>& free : carry.garbage) {
            frame.garbage.push_back(std::move(free));
        }
        frame.garbage.emplace_back([pool = carry.pool] {
            vkDestroyCommandPool(fbe::gpu::device()->device, pool, nullptr);
        });
    }
    carried.clear();
    return true;
}

} // namespace

namespace fbe::gpu {

VkCommandBuffer frame_commands() {
    return begin().commands;
}

VkCommandBuffer upload_commands() {
    return begin().uploads;
}

frame_t& recording_frame() {
    return begin();
}

void after_frame(std::function<void()> free) {
    // Begun first, so what's left goes with this frame, not one whose beginning frees it now.
    begin().garbage.push_back(std::move(free));
}

bool make_frame_commands(const device_t& gpu, frame_t& frame) {
    const VkCommandPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = flag<VkCommandPoolCreateFlags>(VK_COMMAND_POOL_CREATE_TRANSIENT_BIT),
        .queueFamilyIndex = gpu.family,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    VkResult result = vkCreateCommandPool(gpu.device, &pool_info, nullptr, &pool);
    std::array<VkCommandBuffer, 2> buffers{};
    if (result == VK_SUCCESS) {
        const VkCommandBufferAllocateInfo commands_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = static_cast<std::uint32_t>(buffers.size()),
        };
        result = vkAllocateCommandBuffers(gpu.device, &commands_info, buffers.data());
    }
    if (result != VK_SUCCESS) {
        vkDestroyCommandPool(gpu.device, pool, nullptr);
        return fail_vulkan("couldn't make the GPU's frames", result);
    }
    frame.pool = pool;
    frame.uploads = buffers[0];
    frame.commands = buffers[1];
    return true;
}

bool submit_frame(VkSemaphore wait, VkSemaphore signal) {
    device_t& gpu = *device();
    record_graph();
    age_transients();
    frame_t& frame = gpu.frames[gpu.frame];
    if (!frame.begun && wait == VK_NULL_HANDLE && carried.empty()) {
        // Nothing was recorded, so no frame begins to free what the finished ones left.
        for (frame_t& finished : gpu.frames) {
            if (!finished.begun && vkGetFenceStatus(gpu.device, finished.done) == VK_SUCCESS) {
                collect(finished);
            }
        }
        return true;
    }
    begin();
    const bool submitted = submit(gpu, frame, wait, signal);
    gpu.frame = (gpu.frame + 1) % frames_in_flight;
    return submitted;
}

bool finish_frame() {
    device_t& gpu = *device();
    record_graph();
    frame_t& frame = gpu.frames[gpu.frame];
    if (!frame.begun && carried.empty()) {
        return true;
    }
    begin();
    if (!submit(gpu, frame, VK_NULL_HANDLE, VK_NULL_HANDLE)) {
        return false;
    }
    const VkResult waited = vkWaitForFences(gpu.device, 1, &frame.done, VK_TRUE, UINT64_MAX);
    return waited == VK_SUCCESS ? true : fail_vulkan("the GPU couldn't finish the frame", waited);
}

void stop_frames() {
    device_t& gpu = *device();
    vkDeviceWaitIdle(gpu.device);
    for (frame_t& frame : gpu.frames) {
        if (frame.begun) {
            vkEndCommandBuffer(frame.uploads);
            vkEndCommandBuffer(frame.commands);
            frame.begun = false;
        }
        collect(frame);
        // What freeing left goes too.
        collect(frame);
        vmaDestroyBuffer(gpu.allocator, frame.ring.buffer, frame.ring.memory);
        frame.ring = {};
        frame.ring_data = nullptr;
        frame.ring_used = 0;
    }
    for (const carried_t& carry : carried) {
        drop(gpu, carry);
    }
    carried.clear();
    gpu.frame = 0;
}

} // namespace fbe::gpu
