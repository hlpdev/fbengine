# fbengine's own DLL, which the fbengine package's FB code calls through fbengine.h.
fbengine_add_native(fbengine_native fbengine fbe_ fbengine.cpp window.cpp)
target_include_directories(fbengine_native PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(fbengine_native PRIVATE FBE_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}")
target_link_libraries(fbengine_native PRIVATE SDL3::SDL3-static)
