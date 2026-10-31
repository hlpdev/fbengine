# fbengine_physics' DLL, which the fbengine_physics package's FB code calls through
# fbengine_physics.h, an area of its C API to a folder: physics/, and the ones E10 adds. Jolt is
# linked into it, with its own job system.
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
# Jolt's own asserts in a debug build, which catch a call it doesn't take.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(USE_ASSERTS ON CACHE BOOL "" FORCE)
else()
  set(USE_ASSERTS OFF CACHE BOOL "" FORCE)
endif()
# No GPU compute: fbengine's renderer is the GPU's user.
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
# x86-64's SSE4.2 and no more, which every CPU of the GPUs fbengine runs on has; AVX2 and FMA
# would leave some out, and a fused multiply-add rounds otherwise than a multiply and an add.
foreach(option USE_AVX USE_AVX2 USE_AVX512 USE_LZCNT USE_TZCNT USE_F16C USE_FMADD)
  set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(
  JoltPhysics
  GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
  GIT_TAG v5.6.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR Build
  SYSTEM
)
FetchContent_MakeAvailable(JoltPhysics)
# It goes into a DLL.
set_target_properties(Jolt PROPERTIES POSITION_INDEPENDENT_CODE ON)

file(GLOB fbengine_physics_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*/*.cpp")
fbengine_add_native(fbengine_physics_native fbengine_physics fbe_ ${fbengine_physics_sources})
target_include_directories(fbengine_physics_native PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(fbengine_physics_native PRIVATE FBE_PHYSICS_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}")
target_link_libraries(fbengine_physics_native PRIVATE Jolt)
