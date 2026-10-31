# fbengine_tools' DLL, the cooker's and the build's other tools' native half, which the
# fbengine_tools package's FB code calls through fbengine_tools.h, an area of its C API to a folder:
# tools/ and cook/. libktx's encoder and meshoptimizer are linked into it; a game's release doesn't
# ship it. fbengine's res://, jobs, and glTF and image loaders are built into it too, which the
# cooker checks a glTF with as the engine loads it, and its UTF-8 paths on the disk; stb_image's
# code is the cooker's own.
file(GLOB fbengine_tools_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*/*.cpp")
set(fbengine_tools_engine_sources
    "${PROJECT_SOURCE_DIR}/native/fbengine/assets/gltf.cpp"
    "${PROJECT_SOURCE_DIR}/native/fbengine/assets/image.cpp"
    "${PROJECT_SOURCE_DIR}/native/fbengine/jobs/jobs.cpp"
    "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/disk.cpp"
    "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/folder.cpp"
    "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/pak.cpp"
    "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/vfs.cpp")
fbengine_add_native(fbengine_tools_native fbengine_tools fbe_ ${fbengine_tools_sources}
                    ${fbengine_tools_engine_sources})
target_include_directories(fbengine_tools_native PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                           "${PROJECT_SOURCE_DIR}/native/fbengine")
target_compile_definitions(fbengine_tools_native PRIVATE FBE_TOOLS_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}")
target_link_libraries(fbengine_tools_native PRIVATE fbengine_stb ktx fastgltf::fastgltf meshoptimizer)
# fastgltf's exporter is built as fastgltf is, without _UCRT, as cook/glb.hpp says why.
if(MINGW)
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/cook/glb.cpp" TARGET_DIRECTORY
                              fbengine_tools_native PROPERTIES COMPILE_OPTIONS "-U_UCRT")
endif()
