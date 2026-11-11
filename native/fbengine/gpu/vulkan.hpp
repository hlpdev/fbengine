#pragma once

// What the GPU's own files share: Vulkan through volk, VMA, the device the GPU started on, its
// frames in flight, and the bindless set every pipeline reads textures through.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>
#include <volk.h>

// VMA calls Vulkan through the functions volk loaded.
#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

namespace fbe::gpu {

/// How many frames the CPU records while the GPU draws earlier ones.
constexpr std::uint32_t frames_in_flight = 2;

/// Vulkan's flag bits are enum constants, which are signed; its flags are unsigned.
template <typename Flags, typename Bit>
constexpr Flags flag(Bit bit) {
    return static_cast<Flags>(bit);
}

/// A buffer on the GPU, its memory, size, what it's read as, and its address, which shaders read
/// it through (gpu/buffer.hpp).
struct buffer_t {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;
    VkDeviceAddress address = 0;
};

/// One frame in flight: its commands, uploads first, what says the GPU's done with them, and what
/// waits for that to be freed.
struct frame_t {
    VkCommandPool pool = VK_NULL_HANDLE;
    /// Copies into textures and buffers, submitted before the frame's own commands, so a draw
    /// anywhere in the frame sees them.
    VkCommandBuffer uploads = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    /// The frame's first timestamp, submitted before everything else, uploads carried from a
    /// failed submit too, so the GPU's time counts them; never carried itself.
    VkCommandBuffer timing = VK_NULL_HANDLE;
    /// Signaled when the GPU has finished the frame's commands, as it starts.
    VkFence done = VK_NULL_HANDLE;
    /// Signaled when the swapchain's image the frame draws into is free.
    VkSemaphore acquired = VK_NULL_HANDLE;
    /// Whether its commands are being recorded, since the first thing that recorded into them.
    bool begun = false;
    /// What's freed once the GPU is done with the frame, and with every frame before it.
    std::vector<std::function<void()>> garbage;
    /// The frame's ring: host-visible memory for what's written every frame (gpu/buffer.hpp),
    /// where the CPU writes it, and how much of it the frame's given out.
    buffer_t ring;
    std::byte* ring_data = nullptr;
    VkDeviceSize ring_used = 0;
    /// Two timestamps, as the frame's commands begin and end, where the device has them, and
    /// whether its last submit wrote them, to be read once the GPU's done with it.
    VkQueryPool timer = VK_NULL_HANDLE;
    bool timed = false;
};

/// A device's name, as far as its NUL, for std::format: libc++'s takes a char array whole, NULs and
/// all, and fbe_error's C string would end at the first of them, before what's said of the device.
inline std::string_view name_of(const VkPhysicalDeviceProperties& properties) {
    return properties.deviceName;
}

/// The device the GPU started on, and what it draws with. Vulkan 1.3's names for dynamic
/// rendering and synchronization2 or 1.2's extensions', whichever the device has.
struct device_t {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    VmaAllocator allocator = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    /// The most anisotropy its samplers take, 0 if it has none.
    float max_anisotropy = 0.0F;
    /// Whether it can draw triangles as lines.
    bool wireframe = false;
    /// Whether a pipeline can hold depth to the depth range instead of clipping there.
    bool depth_clamp = false;
    /// The furthest its samplers bias a texture's mipmaps, 0 if they can't, as Metal's can't.
    float max_lod_bias = 0.0F;
    /// How many bits of a timestamp its queue writes, 0 if it writes none, and how many
    /// nanoseconds one counts.
    std::uint32_t timestamp_bits = 0;
    float timestamp_period = 0.0F;
    PFN_vkCmdBeginRendering begin_rendering = nullptr;
    PFN_vkCmdEndRendering end_rendering = nullptr;
    PFN_vkCmdPipelineBarrier2 pipeline_barrier = nullptr;
    PFN_vkQueueSubmit2 queue_submit = nullptr;
    std::array<frame_t, frames_in_flight> frames{};
    /// The frame being recorded.
    std::uint32_t frame = 0;
};

/// Vulkan's instance, made the first time something asks for it, with SDL's surfaces' extensions;
/// VK_NULL_HANDLE, with fbe_error's why, if Vulkan can't start.
VkInstance instance();

/// The device, once the GPU has started; nullptr before.
device_t* device();

/// Starts the GPU, making `surface` the swapchain's if there is one, which the device has to be
/// able to present to; false, with fbe_error's why, if it can't, and then it never starts.
bool start(VkSurfaceKHR surface);

/// Stops the GPU, if it's started, tearing everything down in order: the window's images and
/// surface, the pipelines, the work-now commands, the frames, VMA, the device, the validation
/// messenger, and the instance. It can start again.
void stop();

/// How many of VMA's allocations were left when the GPU last stopped, which is 0 unless something
/// wasn't freed.
std::uint32_t allocations_left();

/// VMA's allocations now, and their bytes, as its statistics count them; none before the GPU
/// starts.
struct allocations_t {
    std::uint32_t count = 0;
    std::uint64_t bytes = 0;
    bool operator==(const allocations_t&) const = default;
};
allocations_t allocations();

/// Makes a frame a command pool of its own and its three command buffers, uploads, its own, and
/// its first timestamp's; false,
/// with fbe_error's why, if they can't be, and then it's left as it was.
bool make_frame_commands(const device_t& gpu, frame_t& frame);

/// Makes a frame's fence, signaled, and its acquire semaphore again, after a submit that failed
/// left them waiting on nothing; false, with fbe_error's why, if they can't be.
bool remake_frame(const device_t& gpu, frame_t& frame);

/// What each part of the GPU tears down as it stops: the window's images and surface, the
/// pipelines and their cache, the work-now commands, the textures, the bindless set and its
/// samplers, and what every frame left to free.
void stop_window();
void stop_pipelines();
void stop_immediate();
void stop_textures();
void stop_bindless();
void stop_frames();

/// What starts with the device: the bindless set and its samplers, then the textures, with the
/// white one every unset layer reads; false, with fbe_error's why, if they can't.
bool start_bindless();
bool start_textures();

/// The frame's own commands, which the frame graph records its passes into, and its uploads',
/// begun the first time something records into them after a Flip: that waits for the GPU to
/// finish the frame in flight that last had them, and frees what it left.
VkCommandBuffer frame_commands();
VkCommandBuffer upload_commands();

/// The frame being recorded, begun if it isn't.
frame_t& recording_frame();

/// Has `free` run once the GPU is done with the frame being recorded, and every frame before it.
void after_frame(std::function<void()> free);
/// How many of those wait now.
std::size_t waiting_frees();

/// Has `free` run each time the GPU stops, before it finishes what's freed after frames: what's
/// kept on the GPU from frame to frame goes there. Asked for twice, it's still once.
void at_stop(void (*free)());

/// Records the frame graph, then ends the frame's commands, if any were recorded, and submits
/// them, uploads first, waiting on `wait` and signaling `signal` if they're not VK_NULL_HANDLE, so
/// the next frame in flight is recorded next; false, with fbe_error's why, if the submit failed,
/// which makes the frame's fence and acquire semaphore again.
bool submit_frame(VkSemaphore wait, VkSemaphore signal);

/// Records the frame graph, submits what the frame has so far, and waits for the GPU to finish it,
/// as a readback needs; the frame goes on, recorded again from where it was; false, with
/// fbe_error's why, if it couldn't.
bool finish_frame();

/// The samplers' binding in the bindless set, multisampled images', and the textures', which is
/// last as how many it has varies. A shader reads a texture by its index in the textures' and a
/// sampler's in the samplers', as bindless.slang does.
constexpr std::uint32_t sampler_binding = 0;
constexpr std::uint32_t samples_binding = 1;
constexpr std::uint32_t texture_binding = 2;
/// How many samplers the set has: one for each filter and wrap in U and V, and room.
constexpr std::uint32_t sampler_slots = 32;
/// The fewest textures fbengine needs a device's set to hold, which every device's set has.
constexpr std::uint32_t fewest_textures = 1024;
/// How many multisampled images the set has, the engine's own whose samples a shader reads: one for
/// each size of an HDR camera's image at once, but for one with temporal antialiasing; what's past
/// that reads its average.
constexpr std::uint32_t samples_slots = 64;
/// A multisampled image with no slot.
constexpr std::uint32_t no_samples_slot = UINT32_MAX;

/// The bindless set's layout, which every pipeline's layout has as its set 0, and the set.
VkDescriptorSetLayout bindless_layout();
VkDescriptorSet bindless_set();

/// How many textures the set holds, which is how many there can be.
std::uint32_t texture_capacity();
/// How many images the set's pool was made for: as many as the set has, or allocating it may fail
/// on a GPU that, unlike lavapipe, holds a pool to its sizes.
std::uint32_t pool_images();

/// Points the set's texture `index` at `view`, sampled in SHADER_READ_ONLY_OPTIMAL.
void write_texture(std::uint32_t index, VkImageView view);
/// Points every one of the set's textures at `view`, as write_texture does, so an index no texture
/// has samples it, not whatever a descriptor never written holds, which a real device faults on.
void write_every_texture(VkImageView view);

/// A slot in the set for a multisampled image, with its view, for a shader to read its samples by;
/// no_samples_slot if they're all taken. A slot given back is for another image, once the GPU's
/// done with what it was.
std::uint32_t take_samples_slot(VkImageView view);
void give_samples_slot(std::uint32_t slot);
/// How many of the set's slots for samples are free.
std::uint32_t free_samples_slot_count();
/// Points every free slot for samples at `view`, and those given back from now on, a multisampled
/// image in SHADER_READ_ONLY_OPTIMAL that's there as long as it is; VK_NULL_HANDLE, before it goes,
/// for none.
void point_free_samples_at(VkImageView view);
/// What a slot for samples points at, and what a free one does: for tests.
VkImageView samples_slot_view(std::uint32_t slot);
VkImageView free_samples_view();

/// The screen, for a window `window` pixels big: made again if the size it should be, its own or
/// the window's, or its samples changed, its handle, or the old one's, or 0, if it can't be made.
std::uint64_t update_screen(VkExtent2D window);
/// The part of a window `window` pixels big the screen there is now is shown in, as it's scaled.
VkRect2D screen_area(VkExtent2D window);
/// The screen's handle, 0 without one, and whether it's scaled smoothly.
std::uint64_t screen_handle();
bool screen_smooth();
/// Lets go of the screen as the window closes, or forgets it as stopping frees every texture.
void close_screen();
void stop_screen();
/// A headless window's image as the last Flip left it, RGBA, rows top first; empty without one.
std::vector<std::uint8_t> read_window();
/// Whether a window's open, which the GPU draws the screen for.
bool window_open();
/// For a test: what makes a shown window's surface in SDL's place, so its swapchain's made even on
/// SDL's offscreen video, which shows none; nullptr for SDL's.
void make_surfaces_by(VkSurfaceKHR (*make)());

/// The index of the sampler for a filter and a wrap in U and V, fbe_texture.h's.
std::uint32_t sampler_index(std::int32_t filter, std::int32_t wrap_u, std::int32_t wrap_v);
/// What's left of the LOD bias after what the samplers can take, for a shader to add: all of it on
/// a GPU whose samplers have none, as Metal's through MoltenVK haven't.
float lod_bias_left();
/// How much of the LOD bias is in every sampler.
float lod_bias_in_samplers();
/// The index of 2D's sampler for a filter, clamped or repeating, which has no LOD bias or
/// anisotropy.
std::uint32_t sampler_2d_index(std::int32_t filter, bool repeat);

/// Failures the tests make happen: the next time the GPU does that, it fails, as a driver can.
enum class fault_t : std::uint8_t {
    NONE,
    /// Making an image's view.
    VIEW,
    /// Listing the devices: there are none, as with no driver, until the GPU's stopped.
    NO_DRIVER,
    /// Submitting a frame.
    SUBMIT,
    /// Making the device.
    DEVICE,
    /// Making the commands for work run now.
    WORK_NOW,
    /// Making a sampler.
    SAMPLER,
    /// Submitting a frame, or asking whether one's done, as the device is lost.
    LOST,
    /// Listing the devices, as if each lacked dynamic rendering.
    LACKS,
    /// Making a buffer to upload a texture through.
    UPLOAD,
    /// Making the device, as if its samplers couldn't bias a texture's mipmaps.
    NO_LOD_BIAS,
    /// Making the bindless set, as if the device promised a set 1212 descriptors, as MoltenVK does
    /// on a Mac, and said it held no more.
    PROMISED_DESCRIPTORS,
    /// Making the bindless set, as if the device held one texture fewer than fbengine needs.
    FEWEST_DESCRIPTORS,
    /// Choosing what files' images are transcoded to, as if the device read no compressed format.
    NO_COMPRESSED,
    /// Making a texture of a file's image, as if the device couldn't sample its format.
    NO_FILE_FORMAT,
    /// Making a multisampled image of the engine's, as if the device couldn't read its samples.
    NO_READ_SAMPLES,
    /// Running work now, as a read or a buffer's upload does, as the device is lost.
    LOST_NOW,
};

/// Makes the next `fault` happen.
void inject(fault_t fault);

/// Whether `fault` is to happen now, which it then won't again.
bool faulted(fault_t fault);

/// Starts the GPU, if it hasn't started, with a surface and a swapchain for the window if there's
/// one; false, with fbe_error's why, if it can't.
bool start_with_window();

/// Keeps a Vulkan call's failure for fbe_error(), with what was being done, and returns false.
bool fail_vulkan(std::string_view doing, VkResult result);

/// Whether the GPU was lost, as a driver's crash or reset loses it: nothing runs on it from then
/// on, and what needs it fails, saying so, until it's stopped and started again.
bool lost();
/// The first error the validation layer gave since the program started, as it said it, "" if it's
/// given none: what a test that counts them says when its count's not 0.
const char* first_validation_error();
/// Whether the validation layer checks the GPU's synchronization too, as it does wherever it
/// validates and can.
bool validating_synchronization();
/// Keeps that the GPU was lost if `result` says it was; whether it was.
bool lose(VkResult result);
/// Keeps for fbe_error() that the GPU was lost, and returns false.
bool fail_lost();

/// Counts what the frame does, for the counters a debug overlay shows (fbe_gpu_counters): draws
/// and their triangles, as passes record them, which they count themselves; the graph's passes
/// and barriers, as it's recorded; and the bytes uploaded, as textures and buffers are written.
void count_draws(std::uint64_t draws, std::uint64_t triangles);
void count_passes(std::uint32_t passes, std::uint32_t barriers);
void count_upload(std::uint64_t bytes);

/// How many frames have been submitted, which tells one frame from the next.
std::uint64_t submitted_frames();

} // namespace fbe::gpu
