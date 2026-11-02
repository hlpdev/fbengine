# Vulkan, which fbengine_native draws with: Khronos's headers, volk, which loads the system's
# loader as the GPU starts, so the DLL links no Vulkan library, and VMA, which allocates its memory.
FetchContent_Declare(
  VulkanHeaders
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG v1.4.350
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_Declare(
  volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG 1.4.350
  GIT_SHALLOW TRUE
  SYSTEM
)
# VMA is one header; its own CMake project is for its samples, so it's left out.
FetchContent_Declare(
  VulkanMemoryAllocator
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG v3.5.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR none
)
FetchContent_MakeAvailable(VulkanHeaders)
# volk would take an installed Vulkan SDK's headers over these.
set(VULKAN_HEADERS_INSTALL_DIR "${vulkanheaders_SOURCE_DIR}" CACHE PATH "" FORCE)
FetchContent_MakeAvailable(volk VulkanMemoryAllocator)
add_library(fbengine_vma INTERFACE)
target_include_directories(fbengine_vma SYSTEM INTERFACE
                           "${vulkanmemoryallocator_SOURCE_DIR}/include")

# Built into a shared library, volk's code is position-independent too.
set_target_properties(volk PROPERTIES POSITION_INDEPENDENT_CODE ON)
