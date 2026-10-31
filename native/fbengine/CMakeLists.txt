# fbengine's own DLL, which the fbengine package's FB code calls through fbengine.h, an area of
# its C API to a folder: core/, window/, frame/, and the ones each milestone adds.
file(GLOB fbengine_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*/*.cpp")
fbengine_add_native(fbengine_native fbengine fbe_ ${fbengine_sources})
target_include_directories(fbengine_native PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(fbengine_native PRIVATE FBE_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}")
target_link_libraries(fbengine_native PRIVATE SDL3::SDL3-static)
# The GPU's Vulkan, through volk and VMA.
target_link_libraries(fbengine_native PRIVATE volk Vulkan::Headers fbengine_vma)

# The same sources as a static library, for the C++ tests of what the DLL doesn't export.
if(FBENGINE_BUILD_TESTS)
  add_library(fbengine_internal STATIC ${fbengine_sources})
  fbengine_set_warnings(fbengine_internal)
  target_include_directories(fbengine_internal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_definitions(fbengine_internal PUBLIC FBE_BUILDING
                             FBE_VERSION="${PROJECT_VERSION}")
  target_link_libraries(fbengine_internal PUBLIC SDL3::SDL3-static volk Vulkan::Headers
                        fbengine_vma)
endif()

# The engine's own shaders, embedded in the DLL, and in its sources' static library for the tests.
set(fbengine_shader_targets fbengine_native)
if(TARGET fbengine_internal)
  list(APPEND fbengine_shader_targets fbengine_internal)
endif()
fbengine_add_shaders(TARGETS ${fbengine_shader_targets} NAMESPACE fbe::shaders
                     HEADER engine_shaders.hpp SHADERS shaders/present.slang
                     shaders/canvas.slang)
