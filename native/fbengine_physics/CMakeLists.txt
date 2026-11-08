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
# Jolt's own asserts and debug symbols in a debug build, which catch a call it doesn't take; a
# release's DLL has neither.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(USE_ASSERTS ON CACHE BOOL "" FORCE)
  set(GENERATE_DEBUG_SYMBOLS ON CACHE BOOL "" FORCE)
else()
  set(USE_ASSERTS OFF CACHE BOOL "" FORCE)
  set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "" FORCE)
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
# A world given the same calls steps the same on every target, compiler, and build, as replays
# and games played in lockstep over a network need, for about 8% of Jolt's speed.
set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
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

# Jolt 5.6.0's Jolt/Core/Mutex.h (e77f175, lines 107-111) is patched on MinGW, where its Mutex and
# SharedMutex, std::mutex and std::shared_mutex, leaked: winpthreads allocates each as it's first
# locked, and libstdc++ never destroys them, so a world's 64 body mutexes, about 18 KB, stayed
# after it was freed. There they're slim reader/writer locks, cmake/jolt/mutex_mingw.cpp's, which
# allocate nothing. The patched copy comes before Jolt's own folder, and the patch's text has to be
# there, once, or configuring fails.
if(MINGW)
  file(READ "${joltphysics_SOURCE_DIR}/Jolt/Core/Mutex.h" fbengine_jolt_mutex)
  set(fbengine_jolt_mutex_from
      "#else\n\n// On other platforms just use the STL implementation\nusing MutexBase = mutex;")
  string(FIND "${fbengine_jolt_mutex}" "${fbengine_jolt_mutex_from}" fbengine_first)
  string(FIND "${fbengine_jolt_mutex}" "${fbengine_jolt_mutex_from}" fbengine_last REVERSE)
  if(fbengine_first EQUAL -1 OR NOT fbengine_first EQUAL fbengine_last)
    message(FATAL_ERROR "Jolt/Core/Mutex.h doesn't have, once, what its patch expects")
  endif()
  string(CONCAT fbengine_jolt_mutex_to
         "#elif defined(__MINGW32__)\n\n"
         "// fbengine's patch: a slim reader/writer lock each, which allocates nothing, as\n"
         "// cmake/jolt/mutex_mingw.cpp defines them.\n"
         "class MutexBase : public NonCopyable\n{\npublic:\n"
         "\tbool try_lock();\n\tvoid lock();\n\tvoid unlock();\n\n"
         "private:\n\tvoid *mLock = nullptr;\n};\n\n"
         "class SharedMutexBase : public NonCopyable\n{\npublic:\n"
         "\tbool try_lock();\n\tvoid lock();\n\tvoid unlock();\n"
         "\tbool try_lock_shared();\n\tvoid lock_shared();\n\tvoid unlock_shared();\n\n"
         "private:\n\tvoid *mLock = nullptr;\n};\n\n"
         "${fbengine_jolt_mutex_from}")
  string(REPLACE "${fbengine_jolt_mutex_from}" "${fbengine_jolt_mutex_to}" fbengine_jolt_mutex
                 "${fbengine_jolt_mutex}")
  set(fbengine_jolt_patched "${CMAKE_BINARY_DIR}/jolt-patched")
  file(WRITE "${fbengine_jolt_patched}/Jolt/Core/Mutex.h.in" "${fbengine_jolt_mutex}")
  configure_file("${fbengine_jolt_patched}/Jolt/Core/Mutex.h.in"
                 "${fbengine_jolt_patched}/Jolt/Core/Mutex.h" COPYONLY)
  target_include_directories(Jolt BEFORE PUBLIC "$<BUILD_INTERFACE:${fbengine_jolt_patched}>")
  target_sources(Jolt PRIVATE "${PROJECT_SOURCE_DIR}/cmake/jolt/mutex_mingw.cpp")
endif()

file(GLOB fbengine_physics_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*/*.cpp")
fbengine_add_native(fbengine_physics_native fbengine_physics fbe_ ${fbengine_physics_sources})
target_include_directories(fbengine_physics_native PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(fbengine_physics_native PRIVATE FBE_PHYSICS_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}")
target_link_libraries(fbengine_physics_native PRIVATE Jolt)
# What it works out for Jolt rounds the same everywhere: no multiply and add fused into one.
if(MSVC)
  target_compile_options(fbengine_physics_native PRIVATE /fp:precise)
else()
  target_compile_options(fbengine_physics_native PRIVATE -ffp-contract=off)
endif()
