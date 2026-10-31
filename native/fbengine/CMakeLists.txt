# fbengine's own DLL, which the fbengine package's FB code calls through fbengine.h, an area of
# its C API to a folder: core/, window/, frame/, and the ones each milestone adds.
file(GLOB fbengine_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*/*.cpp")
fbengine_add_native(fbengine_native fbengine fbe_ ${fbengine_sources})
target_include_directories(fbengine_native PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(fbengine_native PRIVATE FBE_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}")
target_link_libraries(fbengine_native PRIVATE SDL3::SDL3-static)
target_link_libraries(fbengine_native PRIVATE fbengine_stb ktx_read fastgltf::fastgltf)
# Its float arithmetic as written, never fused, so every target computes the same: arm64's
# compilers fuse multiply-adds, which rounded a terrain pick there another way.
if(NOT MSVC)
  target_compile_options(fbengine_native PRIVATE -ffp-contract=off)
endif()
