# fbengine_tools' DLL, the cooker's and the build's other tools' native half, which the
# fbengine_tools package's FB code calls through fbengine_tools.h, an area of its C API to a folder:
# tools/ and cook/. libktx's encoder and meshoptimizer are linked into it; a game's release doesn't
# ship it. fbengine's vfs/disk.cpp is built into it too, for its UTF-8 paths on the disk.
file(GLOB fbengine_tools_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*/*.cpp")
fbengine_add_native(fbengine_tools_native fbengine_tools fbe_ ${fbengine_tools_sources}
                    "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/disk.cpp")
target_include_directories(fbengine_tools_native PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                           "${PROJECT_SOURCE_DIR}/native/fbengine")
target_compile_definitions(fbengine_tools_native PRIVATE FBE_TOOLS_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}")
target_link_libraries(fbengine_tools_native PRIVATE fbengine_stb ktx)
