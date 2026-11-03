# fbengine's own DLL, which the fbengine package's FB code calls through fbengine.h, an area of
# its C API to a folder: core/, window/, frame/, and the ones each milestone adds; text/ reads
# fonts with FreeType, HarfBuzz, and SheenBidi (cmake/text.cmake).
file(GLOB fbengine_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*/*.cpp")
fbengine_add_native(fbengine_native fbengine fbe_ ${fbengine_sources})
target_include_directories(fbengine_native PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(fbengine_native PRIVATE FBE_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}")
# What it links, which the tests' static library of its sources links too: SDL; FreeType,
# HarfBuzz, and SheenBidi for text; stb, KTX, and fastgltf for assets; and the GPU's Vulkan,
# through volk and VMA.
set(fbengine_links SDL3::SDL3-static freetype fbengine_harfbuzz fbengine_sheenbidi fbengine_stb
    ktx_read fastgltf::fastgltf volk Vulkan::Headers fbengine_vma)
target_link_libraries(fbengine_native PRIVATE ${fbengine_links})
# Its float arithmetic as written, never fused, so it rounds alike on every target, but for what
# a target's own libm gives: arm64's compilers fuse multiply-adds, which rounded a terrain pick
# there another way.
if(NOT MSVC)
  target_compile_options(fbengine_native PRIVATE -ffp-contract=off)
endif()

# The same sources as a static library, for the C++ tests of what the DLL doesn't export, built
# and linked as the DLL is.
if(FBENGINE_BUILD_TESTS)
  add_library(fbengine_internal STATIC ${fbengine_sources})
  fbengine_set_warnings(fbengine_internal)
  target_include_directories(fbengine_internal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_definitions(fbengine_internal PUBLIC FBE_BUILDING
                             FBE_VERSION="${PROJECT_VERSION}")
  target_link_libraries(fbengine_internal PUBLIC ${fbengine_links})
  if(NOT MSVC)
    target_compile_options(fbengine_internal PRIVATE -ffp-contract=off)
  endif()
endif()

# The engine's own shaders, embedded in the DLL, and in its sources' static library for the tests.
set(fbengine_shader_targets fbengine_native)
if(TARGET fbengine_internal)
  list(APPEND fbengine_shader_targets fbengine_internal)
endif()
fbengine_add_shaders(TARGETS ${fbengine_shader_targets} NAMESPACE fbe::shaders
                     HEADER engine_shaders.hpp SHADERS shaders/present.slang
                     shaders/canvas.slang shaders/environment.slang shaders/scene.slang
                     shaders/tonemap.slang)
