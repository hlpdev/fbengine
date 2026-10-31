// The frame's commands: begun the first time something records after a Flip, the frame graph
// recorded into them as they're submitted, at the Flip, uploads first, and what each frame in
// flight leaves to free once the GPU's done with it. A submit that fails for want of memory keeps
// the frame's uploads, and what they copy from, for the next submit to run first, so a texture
// written then still gets its pixels; a lost GPU runs nothing, freeing what each frame leaves at
// once. And the counters of what the last frame did, its GPU time read from its timestamps once
// the GPU's done with it.

#include "core/engine.hpp"
#include "fbengine.h"
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

/// What the frame being recorded has done so far, what the last frame did, and how long the GPU
/// took over the last frame its timestamps were read for, -1 before.
fbe_gpu_counters_t counting{};
fbe_gpu_counters_t counted{ .gpu_milliseconds = -1.0F };
float gpu_time = -1.0F;

/// Reads how long the GPU took over the frame's last submit, once it's done with it.
void read_timer(const fbe::gpu::device_t& gpu, fbe::gpu::frame_t& frame) {
    if (!frame.timed) {
        return;
    }
    frame.timed = false;
    std::array<std::uint64_t, 2> stamps{};
    const VkResult read = vkGetQueryPoolResults(gpu.device, frame.timer, 0, 2, sizeof(stamps),
                                                stamps.data(), sizeof(std::uint64_t),
                                                flag<VkQueryResultFlags>(VK_QUERY_RESULT_64_BIT));
    if (read != VK_SUCCESS) {
        return;
    }
    const std::uint64_t mask = gpu.timestamp_bits >= 64
                                   ? ~std::uint64_t{ 0 }
                                   : (std::uint64_t{ 1 } << gpu.timestamp_bits) - 1;
    const std::uint64_t ticks = (stamps[1] - stamps[0]) & mask;
    gpu_time = static_cast<float>(static_cast<double>(ticks) * gpu.timestamp_period / 1.0e6);
}

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
    fbe::gpu::lose(vkWaitForFences(gpu.device, 1, &frame.done, VK_TRUE, UINT64_MAX));
    collect(frame);
    read_timer(gpu, frame);
    vkResetCommandPool(gpu.device, frame.pool, 0);
    const VkCommandBufferBeginInfo info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = flag<VkCommandBufferUsageFlags>(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT),
    };
    vkBeginCommandBuffer(frame.uploads, &info);
    vkBeginCommandBuffer(frame.commands, &info);
    if (frame.timer != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(frame.commands, frame.timer, 0, 2);
        vkCmdWriteTimestamp(frame.commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame.timer, 0);
    }
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
    if (frame.timer != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(frame.commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timer, 1);
    }
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
    VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (fbe::gpu::faulted(fbe::gpu::fault_t::LOST)) {
        result = VK_ERROR_DEVICE_LOST;
    } else if (!fbe::gpu::faulted(fbe::gpu::fault_t::SUBMIT)) {
        result = gpu.queue_submit(gpu.queue, 1, &info, frame.done);
    }
    frame.timed = result == VK_SUCCESS && frame.timer != VK_NULL_HANDLE;
    if (result != VK_SUCCESS) {
        fbe::gpu::lose(result);
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

/// Gives up what a lost GPU's frame recorded, and frees what it and what was carried left now, as
/// nothing will run; false, with fbe_error's why.
bool give_up(const fbe::gpu::device_t& gpu, fbe::gpu::frame_t& frame) {
    fbe::gpu::drop_passes();
    if (frame.begun) {
        vkEndCommandBuffer(frame.uploads);
        vkEndCommandBuffer(frame.commands);
        frame.begun = false;
    }
    vkDeviceWaitIdle(gpu.device);
    collect(frame);
    for (const carried_t& carry : carried) {
        drop(gpu, carry);
    }
    carried.clear();
    return fbe::gpu::fail_lost();
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

void count_draws(std::uint64_t draws, std::uint64_t triangles) {
    counting.draws += static_cast<std::int64_t>(draws);
    counting.triangles += static_cast<std::int64_t>(triangles);
}

void count_passes(std::uint32_t passes, std::uint32_t barriers) {
    counting.passes += static_cast<std::int32_t>(passes);
    counting.barriers += static_cast<std::int32_t>(barriers);
}

void count_upload(std::uint64_t bytes) {
    counting.uploaded += static_cast<std::int64_t>(bytes);
}

bool submit_frame(VkSemaphore wait, VkSemaphore signal) {
    device_t& gpu = *device();
    frame_t& frame = gpu.frames[gpu.frame];
    if (lost()) {
        counted = { .gpu_milliseconds = -1.0F };
        counting = {};
        return give_up(gpu, frame);
    }
    record_graph();
    age_transients();
    // What the frame did, which its submit ends, the GPU's time a frame or two behind.
    counted = counting;
    counted.transient_images = static_cast<std::int32_t>(transient_images());
    counted.gpu_milliseconds = gpu_time;
    counting = {};
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
    // Its ring starts again as the frame's begun next, once the GPU's done with it; a readback's
    // submit, mid-frame, leaves it, as slices handed out before are still the frame's.
    frame.ring_used = 0;
    gpu.frame = (gpu.frame + 1) % frames_in_flight;
    return submitted;
}

bool finish_frame() {
    device_t& gpu = *device();
    frame_t& frame = gpu.frames[gpu.frame];
    if (lost()) {
        return give_up(gpu, frame);
    }
    record_graph();
    if (!frame.begun && carried.empty()) {
        return true;
    }
    begin();
    if (!submit(gpu, frame, VK_NULL_HANDLE, VK_NULL_HANDLE)) {
        return false;
    }
    const VkResult waited = vkWaitForFences(gpu.device, 1, &frame.done, VK_TRUE, UINT64_MAX);
    if (lose(waited)) {
        return fail_lost();
    }
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
    counting = {};
    counted = { .gpu_milliseconds = -1.0F };
    gpu_time = -1.0F;
}

} // namespace fbe::gpu

extern "C" {

FBE_API bool fbe_gpu_counters(fbe_gpu_counters_t* counters) {
    if (counters == nullptr) {
        return fbe::fail("no counters were given to fill");
    }
    *counters = counted;
    return true;
}

} // extern "C"
