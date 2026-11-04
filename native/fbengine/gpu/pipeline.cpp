// Graphics pipelines, their layout and shader modules, and the pipeline cache, which is read as the
// GPU starts and written at the Flip after new pipelines are made.

#include "gpu/pipeline.hpp"

#include "core/engine.hpp"
#include "fbengine.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <SDL3/SDL.h>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace {

using fbe::gpu::flag;

struct hash_t {
    std::size_t operator()(const fbe::gpu::pipeline_desc_t& desc) const {
        std::size_t hash = std::hash<const void*>{}(desc.shader);
        const auto mix = [&hash](std::size_t value) {
            hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
        };
        mix(std::hash<std::string_view>{}(desc.vertex));
        mix(std::hash<std::string_view>{}(desc.fragment));
        mix(static_cast<std::size_t>(desc.color));
        mix(static_cast<std::size_t>(desc.depth));
        mix(static_cast<std::size_t>(desc.samples));
        mix(static_cast<std::size_t>(desc.topology));
        mix(static_cast<std::size_t>(desc.polygon));
        mix(static_cast<std::size_t>(desc.cull));
        mix(static_cast<std::size_t>(desc.front));
        mix(static_cast<std::size_t>(desc.blend));
        mix(static_cast<std::size_t>(desc.depth_test) | (std::size_t{ desc.depth_write } << 1U));
        mix(static_cast<std::size_t>(desc.depth_compare));
        mix(static_cast<std::size_t>(desc.depth_clamp));
        for (std::uint32_t i = 0; i < desc.constant_count; ++i) {
            mix(static_cast<std::size_t>(desc.constants[i]));
        }
        return hash;
    }
};

VkPipelineLayout layout = VK_NULL_HANDLE;

/// The shader modules made, by shader, and the pipelines, by what they are; made as they're first
/// used, as making a map can throw.
std::unordered_map<const fbe::gpu::shader_t*, VkShaderModule>& modules() {
    static std::unordered_map<const fbe::gpu::shader_t*, VkShaderModule> made;
    return made;
}

std::unordered_map<fbe::gpu::pipeline_desc_t, VkPipeline, hash_t>& pipelines() {
    static std::unordered_map<fbe::gpu::pipeline_desc_t, VkPipeline, hash_t> made;
    return made;
}

VkPipelineCache cache = VK_NULL_HANDLE;
std::string cache_path;
bool cache_path_set = false;
/// How many pipelines there were when the cache's data was last looked at.
std::size_t cached = 0;
/// The size and checksum of the cache's data as it was read or last written, which it's written
/// again only once it differs from; and how many bytes were read, and writes made.
std::size_t saved_size = 0;
std::uint64_t saved_sum = 0;
std::size_t loaded = 0;
std::int32_t writes = 0;
/// When the cache was last written, and how long it waits to be written again while the GPU
/// runs, so a game making pipelines as it loads writes once, not at every Flip.
std::uint64_t last_write = 0;
bool written_yet = false;
std::uint64_t interval = 2'000'000'000;

/// The first bytes of fbengine's pipeline cache files, and the version of what follows.
constexpr std::array<char, 8> magic{ 'F', 'B', 'E', 'P', 'I', 'P', 'E', '\0' };
constexpr std::uint32_t format_version = 1;
/// The header: magic, version, vendor, device, driver, the cache's UUID, size, and checksum.
constexpr std::size_t header_size =
    magic.size() + (4 * sizeof(std::uint32_t)) + VK_UUID_SIZE + (2 * sizeof(std::uint64_t));

/// FNV-1a's 64-bit hash, which says a file came back as it was written.
std::uint64_t checksum(std::span<const std::byte> data) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const std::byte byte : data) {
        hash = (hash ^ static_cast<std::uint64_t>(byte)) * 0x100000001b3ULL;
    }
    return hash;
}

template <typename Value>
void put(std::vector<std::byte>& bytes, const Value& value) {
    const auto* from = reinterpret_cast<const std::byte*>(&value);
    bytes.insert(bytes.end(), from, from + sizeof(Value));
}

template <typename Value>
Value take(std::span<const std::byte> bytes, std::size_t& at) {
    Value value{};
    std::memcpy(&value, bytes.data() + at, sizeof(Value));
    at += sizeof(Value);
    return value;
}

/// The running executable's name, without its folder or extension, in UTF-8; "" if it can't be
/// found.
std::string program_name() {
    std::string executable;
#ifdef _WIN32
    std::wstring name(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
    const int size = WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(length), nullptr,
                                         0, nullptr, nullptr);
    executable.resize(static_cast<std::size_t>(std::max(size, 0)));
    WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(length), executable.data(), size,
                        nullptr, nullptr);
#elifdef __APPLE__
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string name(size, '\0');
    if (_NSGetExecutablePath(name.data(), &size) == 0) {
        executable = name.c_str();
    }
#else
    std::error_code error;
    executable = std::filesystem::read_symlink("/proc/self/exe", error).string();
#endif
    const std::size_t slash = executable.find_last_of("/\\");
    std::string stem = slash == std::string::npos ? executable : executable.substr(slash + 1);
    const std::size_t dot = stem.rfind('.');
    if (dot != std::string::npos && dot > 0) {
        stem.resize(dot);
    }
    return stem;
}

/// The user's folder for caches: LOCALAPPDATA on Windows, ~/Library/Caches on macOS, and
/// XDG_CACHE_HOME or ~/.cache elsewhere; "" if the environment doesn't say.
std::string user_cache_folder() {
#ifdef _WIN32
    const char* local = SDL_getenv("LOCALAPPDATA");
    return local != nullptr ? local : "";
#else
    const char* home = SDL_getenv("HOME");
#ifdef __APPLE__
    return home != nullptr ? std::string(home) + "/Library/Caches" : "";
#else
    const char* xdg = SDL_getenv("XDG_CACHE_HOME");
    if (xdg != nullptr && *xdg != '\0') {
        return xdg;
    }
    return home != nullptr ? std::string(home) + "/.cache" : "";
#endif
#endif
}

const std::string& the_cache_path() {
    if (!cache_path_set) {
        cache_path = fbe::gpu::default_pipeline_cache();
        cache_path_set = true;
    }
    return cache_path;
}

std::vector<std::byte> read_file(const std::string& path) {
    std::size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (data == nullptr) {
        return {};
    }
    std::vector<std::byte> bytes(size);
    std::memcpy(bytes.data(), data, size);
    SDL_free(data);
    return bytes;
}

/// Writes `bytes` to `path` whole or not at all, through a file beside it that takes its place.
bool write_file(const std::string& path, std::span<const std::byte> bytes) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos && !SDL_CreateDirectory(path.substr(0, slash).c_str())) {
        return false;
    }
    const std::string partial = path + ".partial";
    return SDL_SaveFile(partial.c_str(), bytes.data(), bytes.size()) &&
           SDL_RenamePath(partial.c_str(), path.c_str());
}

VkShaderModule module_of(const fbe::gpu::device_t& gpu, const fbe::gpu::shader_t& shader) {
    const auto found = modules().find(&shader);
    if (found != modules().end()) {
        return found->second;
    }
    const VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader.code.size_bytes(),
        .pCode = shader.code.data(),
    };
    VkShaderModule module = VK_NULL_HANDLE;
    const VkResult result = vkCreateShaderModule(gpu.device, &info, nullptr, &module);
    if (result != VK_SUCCESS) {
        fbe::gpu::fail_vulkan(std::format("couldn't load the shader {}", shader.name), result);
        return VK_NULL_HANDLE;
    }
    modules().emplace(&shader, module);
    return module;
}

VkPipelineColorBlendAttachmentState blend_state(fbe::gpu::blend_t blend) {
    using fbe::gpu::blend_t;
    VkPipelineColorBlendAttachmentState state{
        .blendEnable = blend == blend_t::NONE ? VK_FALSE : VK_TRUE,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = flag<VkColorComponentFlags>(VK_COLOR_COMPONENT_R_BIT) |
                          flag<VkColorComponentFlags>(VK_COLOR_COMPONENT_G_BIT) |
                          flag<VkColorComponentFlags>(VK_COLOR_COMPONENT_B_BIT) |
                          flag<VkColorComponentFlags>(VK_COLOR_COMPONENT_A_BIT),
    };
    const auto factors = [&state](VkBlendFactor source, VkBlendFactor target,
                                  VkBlendFactor source_alpha, VkBlendFactor target_alpha) {
        state.srcColorBlendFactor = source;
        state.dstColorBlendFactor = target;
        state.srcAlphaBlendFactor = source_alpha;
        state.dstAlphaBlendFactor = target_alpha;
    };
    switch (blend) {
    case blend_t::NONE:
        break;
    case blend_t::ALPHA:
        factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE,
                VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        break;
    case blend_t::PREMULTIPLIED:
        factors(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE,
                VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        break;
    case blend_t::ADD:
        factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO,
                VK_BLEND_FACTOR_ONE);
        break;
    case blend_t::MULTIPLY:
        factors(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO,
                VK_BLEND_FACTOR_ONE);
        break;
    }
    return state;
}

VkPipeline make_pipeline(const fbe::gpu::device_t& gpu, const fbe::gpu::pipeline_desc_t& desc) {
    VkPipelineLayout pipeline_layout = fbe::gpu::pipeline_layout();
    if (pipeline_layout == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    VkShaderModule module = module_of(gpu, *desc.shader);
    if (module == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    const std::string vertex(desc.vertex);
    const std::string fragment(desc.fragment);
    std::array<VkSpecializationMapEntry, 16> entries{};
    for (std::uint32_t i = 0; i < desc.constant_count; ++i) {
        entries[i] = {
            .constantID = i,
            .offset = i * static_cast<std::uint32_t>(sizeof(std::uint32_t)),
            .size = sizeof(std::uint32_t),
        };
    }
    const VkSpecializationInfo specialization{
        .mapEntryCount = desc.constant_count,
        .pMapEntries = entries.data(),
        .dataSize = desc.constant_count * sizeof(std::uint32_t),
        .pData = desc.constants.data(),
    };
    const VkSpecializationInfo* constants = desc.constant_count > 0 ? &specialization : nullptr;
    const std::array stages{
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = module,
            .pName = vertex.c_str(),
            .pSpecializationInfo = constants,
        },
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = module,
            .pName = fragment.c_str(),
            .pSpecializationInfo = constants,
        },
    };
    const VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    const VkPipelineInputAssemblyStateCreateInfo assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = desc.topology,
    };
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const VkPipelineRasterizationStateCreateInfo rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = desc.depth_clamp ? VK_TRUE : VK_FALSE,
        .polygonMode = desc.polygon,
        .cullMode = desc.cull,
        .frontFace = desc.front,
        .lineWidth = 1.0F,
    };
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = desc.samples,
    };
    const VkPipelineDepthStencilStateCreateInfo depth{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = desc.depth_test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = desc.depth_write ? VK_TRUE : VK_FALSE,
        .depthCompareOp = desc.depth_compare,
    };
    const VkPipelineColorBlendAttachmentState attachment = blend_state(desc.blend);
    const bool has_color = desc.color != VK_FORMAT_UNDEFINED;
    const VkPipelineColorBlendStateCreateInfo blending{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = has_color ? 1U : 0U,
        .pAttachments = &attachment,
    };
    const std::array dynamic_states{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    const VkPipelineDynamicStateCreateInfo dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    const VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = has_color ? 1U : 0U,
        .pColorAttachmentFormats = &desc.color,
        .depthAttachmentFormat = desc.depth,
    };
    const VkGraphicsPipelineCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = desc.fragment.empty() ? 1U : 2U,
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth,
        .pColorBlendState = &blending,
        .pDynamicState = &dynamic,
        .layout = pipeline_layout,
    };
    VkPipeline made = VK_NULL_HANDLE;
    const VkResult result = vkCreateGraphicsPipelines(gpu.device, cache, 1, &info, nullptr, &made);
    if (result != VK_SUCCESS) {
        fbe::gpu::fail_vulkan(std::format("couldn't make a pipeline of {}", desc.shader->name),
                              result);
        return VK_NULL_HANDLE;
    }
    return made;
}

} // namespace

namespace fbe::gpu {

VkPipelineLayout pipeline_layout() {
    const device_t* gpu = device();
    if (layout != VK_NULL_HANDLE || gpu == nullptr) {
        return layout;
    }
    const VkPushConstantRange constants{
        .stageFlags = flag<VkShaderStageFlags>(VK_SHADER_STAGE_ALL),
        .size = push_constant_size,
    };
    VkDescriptorSetLayout bindless = bindless_layout();
    const VkPipelineLayoutCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &bindless,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &constants,
    };
    const VkResult result = vkCreatePipelineLayout(gpu->device, &info, nullptr, &layout);
    if (result != VK_SUCCESS) {
        layout = VK_NULL_HANDLE;
        fail_vulkan("couldn't make the pipelines' layout", result);
    }
    return layout;
}

VkPipeline pipeline(const pipeline_desc_t& desc) {
    const auto found = pipelines().find(desc);
    if (found != pipelines().end()) {
        return found->second;
    }
    const device_t* gpu = device();
    if (gpu == nullptr) {
        fail("the GPU hasn't started");
        return VK_NULL_HANDLE;
    }
    VkPipeline made = make_pipeline(*gpu, desc);
    if (made != VK_NULL_HANDLE) {
        pipelines().emplace(desc, made);
    }
    return made;
}

std::vector<std::byte> pipeline_cache_from(const std::string& path,
                                           const VkPhysicalDeviceProperties& properties) {
    return path.empty() ? std::vector<std::byte>{}
                        : unpack_pipeline_cache(read_file(path), properties);
}

void start_pipeline_cache() {
    const device_t* gpu = device();
    const std::vector<std::byte> data = pipeline_cache_from(the_cache_path(), gpu->properties);
    loaded = data.size();
    saved_size = data.size();
    saved_sum = checksum(data);
    written_yet = false;
    const VkPipelineCacheCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = data.size(),
        .pInitialData = data.data(),
    };
    // A cache the driver won't take is no cache: pipelines are made without one.
    if (vkCreatePipelineCache(gpu->device, &info, nullptr, &cache) != VK_SUCCESS) {
        cache = VK_NULL_HANDLE;
    }
    cached = pipelines().size();
}

std::vector<std::byte> pipeline_cache_data() {
    const device_t* gpu = device();
    if (gpu == nullptr || cache == VK_NULL_HANDLE) {
        return {};
    }
    std::size_t size = 0;
    vkGetPipelineCacheData(gpu->device, cache, &size, nullptr);
    std::vector<std::byte> data(size);
    if (vkGetPipelineCacheData(gpu->device, cache, &size, data.data()) != VK_SUCCESS) {
        return {};
    }
    data.resize(size);
    return data;
}

void save_pipeline_cache(bool now) {
    const device_t* gpu = device();
    if (gpu == nullptr || cache == VK_NULL_HANDLE || pipelines().size() == cached) {
        return;
    }
    const std::uint64_t time = SDL_GetTicksNS();
    if (!now && written_yet && time - last_write < interval) {
        return;
    }
    cached = pipelines().size();
    const std::string& path = the_cache_path();
    if (path.empty()) {
        return;
    }
    const std::vector<std::byte> data = pipeline_cache_data();
    const std::uint64_t sum = checksum(data);
    if (data.size() == saved_size && sum == saved_sum) {
        return;
    }
    // A cache that can't be written is only slower next time.
    if (write_file(path, pack_pipeline_cache(data, gpu->properties))) {
        saved_size = data.size();
        saved_sum = sum;
        ++writes;
    }
    last_write = time;
    written_yet = true;
}

bool pipeline_cache_pending() {
    return pipelines().size() != cached;
}

std::size_t pipeline_cache_loaded() {
    return loaded;
}

std::int32_t pipeline_cache_writes() {
    return writes;
}

void set_pipeline_cache_interval(double seconds) {
    interval = static_cast<std::uint64_t>(seconds * 1e9);
}

void stop_pipelines() {
    const device_t* gpu = device();
    save_pipeline_cache(true);
    for (const auto& [desc, made] : pipelines()) {
        vkDestroyPipeline(gpu->device, made, nullptr);
    }
    for (const auto& [shader, module] : modules()) {
        vkDestroyShaderModule(gpu->device, module, nullptr);
    }
    pipelines().clear();
    modules().clear();
    vkDestroyPipelineLayout(gpu->device, layout, nullptr);
    vkDestroyPipelineCache(gpu->device, cache, nullptr);
    layout = VK_NULL_HANDLE;
    cache = VK_NULL_HANDLE;
    cached = 0;
}

std::vector<std::byte> pack_pipeline_cache(std::span<const std::byte> data,
                                           const VkPhysicalDeviceProperties& properties) {
    std::vector<std::byte> bytes;
    bytes.reserve(header_size + data.size());
    put(bytes, magic);
    put(bytes, format_version);
    put(bytes, properties.vendorID);
    put(bytes, properties.deviceID);
    put(bytes, properties.driverVersion);
    put(bytes, properties.pipelineCacheUUID);
    put(bytes, static_cast<std::uint64_t>(data.size()));
    put(bytes, checksum(data));
    bytes.insert(bytes.end(), data.begin(), data.end());
    return bytes;
}

std::vector<std::byte> unpack_pipeline_cache(std::span<const std::byte> file,
                                             const VkPhysicalDeviceProperties& properties) {
    if (file.size() < header_size) {
        return {};
    }
    std::size_t at = 0;
    const auto found_magic = take<std::array<char, 8>>(file, at);
    const auto version = take<std::uint32_t>(file, at);
    const auto vendor = take<std::uint32_t>(file, at);
    const auto device = take<std::uint32_t>(file, at);
    const auto driver = take<std::uint32_t>(file, at);
    const auto uuid = take<std::array<std::uint8_t, VK_UUID_SIZE>>(file, at);
    const auto size = take<std::uint64_t>(file, at);
    const auto sum = take<std::uint64_t>(file, at);
    const std::span<const std::byte> data = file.subspan(at);
    if (found_magic != magic || version != format_version || vendor != properties.vendorID ||
        device != properties.deviceID || driver != properties.driverVersion ||
        std::memcmp(uuid.data(), static_cast<const void*>(properties.pipelineCacheUUID),
                    VK_UUID_SIZE) != 0 ||
        size != data.size() || sum != checksum(data)) {
        return {};
    }
    return { data.begin(), data.end() };
}

std::string default_pipeline_cache() {
    const std::string folder = user_cache_folder();
    const std::string program = program_name();
    if (folder.empty() || program.empty()) {
        return "";
    }
    return std::format("{}/fbengine/{}/pipelines.bin", folder, program);
}

} // namespace fbe::gpu

extern "C" {

FBE_API void fbe_gpu_set_pipeline_cache(const char* path) {
    cache_path = path != nullptr ? path : "";
    cache_path_set = true;
}

FBE_API const char* fbe_gpu_pipeline_cache(void) {
    return the_cache_path().c_str();
}

FBE_API int32_t fbe_gpu_pipelines(void) {
    return static_cast<std::int32_t>(pipelines().size());
}

} // extern "C"
