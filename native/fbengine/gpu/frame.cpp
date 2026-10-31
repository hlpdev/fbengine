// The frame's commands: begun the first time something records after a Flip, submitted at the
// Flip, uploads first, and what each frame in flight leaves to free once the GPU's done with it.

#include "core/engine.hpp"
#include "gpu/vulkan.hpp"

#include <array>
#include <functional>
#include <utility>
#include <vector>

namespace {

using fbe::gpu::flag;

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

/// Ends and submits the frame's commands with its fence; false, with fbe_error's why, if the
/// submit failed, which makes the fence and acquire semaphore again.
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
    const std::array commands{
        VkCommandBufferSubmitInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = frame.uploads,
        },
        VkCommandBufferSubmitInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = frame.commands,
        },
    };
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
        // Nothing will signal the fence or wait on the acquire, so they're made again; what the
        // frame left to free waits for the GPU as a whole.
        vkDeviceWaitIdle(gpu.device);
        collect(frame);
        fbe::gpu::remake_frame(gpu, frame);
        return fbe::gpu::fail_vulkan("the GPU couldn't run the frame", result);
    }
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

void after_frame(std::function<void()> free) {
    // Begun first, so what's left goes with this frame, not one whose beginning frees it now.
    begin().garbage.push_back(std::move(free));
}

bool submit_frame(VkSemaphore wait, VkSemaphore signal) {
    device_t& gpu = *device();
    frame_t& frame = gpu.frames[gpu.frame];
    if (!frame.begun && wait == VK_NULL_HANDLE) {
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
    frame_t& frame = gpu.frames[gpu.frame];
    if (!frame.begun) {
        return true;
    }
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
    }
    gpu.frame = 0;
}

} // namespace fbe::gpu
