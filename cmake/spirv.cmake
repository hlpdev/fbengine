# SPIRV-Tools' validator, which a game's shader is checked by as it's made (shader/shader.cpp),
# static, at the Vulkan SDK release the Vulkan headers are (cmake/vulkan.cmake), with its headers.
# It's built without fbengine's _UCRT (cmake/native.cmake), as the static libstdc++ is, whose
# streams it uses.
FetchContent_Declare(
  SPIRV-Headers
  GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
  GIT_TAG vulkan-sdk-1.4.350.0
  GIT_SHALLOW TRUE
  SYSTEM
  EXCLUDE_FROM_ALL
)
FetchContent_Declare(
  SPIRV-Tools
  GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Tools.git
  GIT_TAG vulkan-sdk-1.4.350.0
  GIT_SHALLOW TRUE
  SYSTEM
  EXCLUDE_FROM_ALL
)
set(SPIRV_HEADERS_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
set(SPIRV_HEADERS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SPIRV-Headers)
set(SPIRV-Headers_SOURCE_DIR "${spirv-headers_SOURCE_DIR}")
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
set(SPIRV_TOOLS_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(SPIRV_TOOLS_LIBRARY_TYPE STATIC CACHE STRING "" FORCE)
FetchContent_MakeAvailable(SPIRV-Tools)
# Built into a shared library, its code is position-independent too.
set_target_properties(SPIRV-Tools-static PROPERTIES POSITION_INDEPENDENT_CODE ON)
