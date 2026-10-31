# The target a package's natives are built for, as FutureBlitz's [native] paths name it:
# windows-x64, linux-x64, linux-arm64, macos-arm64, or macos-x64.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  set(fbengine_arch x64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  set(fbengine_arch arm64)
else()
  message(FATAL_ERROR "fbengine builds for 64-bit x86 and ARM, not ${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(WIN32)
  set(fbengine_os windows)
elseif(APPLE)
  set(fbengine_os macos)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(fbengine_os linux)
else()
  message(FATAL_ERROR "fbengine builds for Windows, Linux, and macOS, not ${CMAKE_SYSTEM_NAME}")
endif()
set(FBENGINE_TARGET "${fbengine_os}-${fbengine_arch}")
if(FBENGINE_TARGET STREQUAL "windows-arm64")
  message(FATAL_ERROR "FutureBlitz has no windows-arm64 target")
endif()

# A package's native DLL, `<name>.dll` with its import library on Windows, `lib<name>.so` on
# Linux, and `lib<name>.dylib`, as @rpath/lib<name>.dylib, on macOS, which each build copies into
# packages/<package>/native/<target>/, where the package's [native] finds it. Only its C API,
# the functions starting with `<prefix>`, is exported, and its C++ runtime is linked into it, so it
# needs nothing but the OS's own libraries.
function(fbengine_add_native target package prefix)
  add_library(${target} SHARED ${ARGN})
  fbengine_set_warnings(${target})
  # On Windows only what's dllexport is exported, and CMake's flag for hidden inlines there is
  # one clang-tidy doesn't take.
  if(NOT WIN32)
    set_target_properties(${target} PROPERTIES CXX_VISIBILITY_PRESET hidden
                          VISIBILITY_INLINES_HIDDEN ON)
  endif()
  if(WIN32)
    set_target_properties(${target} PROPERTIES PREFIX "")
  elseif(APPLE)
    set_target_properties(${target} PROPERTIES INSTALL_NAME_DIR "@rpath")
  endif()

  # MinGW's static winpthread needs UCRT, as programs' does; compiling, -mcrtdll=ucrt only defines
  # _UCRT, which clang-tidy takes and the flag it doesn't. On Linux it exports only its C API, not
  # the C++ runtime linked into it, which a program could otherwise take over. MSVC's and macOS's
  # C++ runtimes are the system's.
  if(MINGW)
    target_compile_definitions(${target} PRIVATE _UCRT)
    target_link_options(${target} PRIVATE -static -mcrtdll=ucrt)
  elseif(APPLE)
    # Static libraries' default-visible symbols, like SDL's, stay in it.
    target_link_options(${target} PRIVATE "LINKER:-exported_symbol,_${prefix}*")
  elseif(UNIX)
    set(map "${CMAKE_CURRENT_BINARY_DIR}/${target}.map")
    file(WRITE "${map}" "{\n    global: ${prefix}*;\n    local: *;\n};\n")
    target_link_options(${target} PRIVATE -static-libstdc++ -static-libgcc
                        "-Wl,--version-script=${map}")
  endif()

  set(folder "${PROJECT_SOURCE_DIR}/packages/${package}/native/${FBENGINE_TARGET}")
  set(files "$<TARGET_FILE:${target}>")
  if(WIN32)
    list(APPEND files "$<TARGET_LINKER_FILE:${target}>")
  endif()
  add_custom_command(TARGET ${target} POST_BUILD
                     COMMAND "${CMAKE_COMMAND}" -E make_directory "${folder}"
                     COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${files} "${folder}"
                     VERBATIM)
endfunction()
