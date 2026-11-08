FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.16.1
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# ctest puts the toolchain's DLLs (MinGW's libstdc++ and friends) on PATH for the tests, so
# they run from any terminal.
set(fbengine_test_dll_paths "")
if(MINGW)
  cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH fbengine_test_dll_paths)
endif()

# The tests' Vulkan: lavapipe, Mesa's driver on the CPU, whose frames are the same on every
# machine, as the only driver the loader sees. On Windows it's mesa-dist-win's MSVC build, CI's,
# fetched; on Linux the system's (apt's mesa-vulkan-drivers), each of its manifests, as a multiarch
# system has one for each architecture and the loader skips the others; on macOS it's MoltenVK,
# the system's. FBENGINE_VULKAN_DRIVER names another.
set(FBENGINE_VULKAN_DRIVER "" CACHE FILEPATH
    "The Vulkan driver manifest (an ICD .json) the tests use, alone; empty for lavapipe")
set(fbengine_vulkan_driver "${FBENGINE_VULKAN_DRIVER}")
if(fbengine_vulkan_driver STREQUAL "" AND WIN32)
  FetchContent_Declare(
    lavapipe
    URL https://github.com/pal1000/mesa-dist-win/releases/download/26.2.4/mesa3d-26.2.4-release-msvc.7z
    URL_HASH SHA256=351fc8c8b695878ffb3eaa044b3ead08672a48b1a045e3c3e3975811df0f6695
    SOURCE_SUBDIR none
  )
  FetchContent_MakeAvailable(lavapipe)
  set(fbengine_vulkan_driver "${lavapipe_SOURCE_DIR}/x64/lvp_icd.x86_64.json")
elseif(fbengine_vulkan_driver STREQUAL "" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  file(GLOB fbengine_lavapipes "/usr/share/vulkan/icd.d/lvp_icd*.json")
  list(JOIN fbengine_lavapipes ":" fbengine_vulkan_driver)
endif()
# Windows' loader takes only backslashes.
set(fbengine_vulkan_environment "")
if(NOT fbengine_vulkan_driver STREQUAL "")
  cmake_path(NATIVE_PATH fbengine_vulkan_driver fbengine_vulkan_driver)
  set(fbengine_vulkan_environment "VK_DRIVER_FILES=${fbengine_vulkan_driver}")
endif()
# Every Catch2 test found at build time sees only the tests' Vulkan driver too.
function(catch_discover_tests)
  if(fbengine_vulkan_environment STREQUAL "")
    _catch_discover_tests(${ARGV})
  else()
    _catch_discover_tests(${ARGV} PROPERTIES ENVIRONMENT "${fbengine_vulkan_environment}")
  endif()
endfunction()

# stb's PNGs, for the golden image harness; stb has no releases, so it's pinned to a commit.
FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5
  SOURCE_SUBDIR none
)
FetchContent_MakeAvailable(stb)

# Vulkan with no window, on the system's loader, which volk loads, and the tests' driver; and the
# GPU through fbengine's C API and its insides, built in from fbengine_internal, with the golden
# image harness (tests/golden/) and the tests' own shaders. Each test's tags are its labels, so
# `ctest -L golden` runs the golden image tests, as tools/golden.py does.
add_executable(fbengine_gpu_tests gpu/vulkan_tests.cpp gpu/present_tests.cpp
               gpu/pipeline_tests.cpp gpu/texture_tests.cpp gpu/target_tests.cpp
               gpu/buffer_tests.cpp gpu/graph_tests.cpp gpu/canvas_tests.cpp
               gpu/environment_tests.cpp
               gpu/pixels_tests.cpp gpu/text_tests.cpp gpu/example_tests.cpp
               gpu/scene_tests.cpp
               golden/golden.cpp golden/golden_tests.cpp golden/stb.cpp)
target_link_libraries(fbengine_gpu_tests PRIVATE volk Vulkan::Headers Catch2::Catch2WithMain
                      fbengine_internal)
target_include_directories(fbengine_gpu_tests SYSTEM PRIVATE "${stb_SOURCE_DIR}")
target_include_directories(fbengine_gpu_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                           "${PROJECT_BINARY_DIR}/native/fbengine/engine_shaders")
target_compile_definitions(fbengine_gpu_tests PRIVATE
                           FBE_TEST_FONTS="${harfbuzz_SOURCE_DIR}/test/api/fonts"
                           FBE_OWN_TEST_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
                           FBENGINE_EXAMPLE_FRAMES="${PROJECT_BINARY_DIR}/example-frames"
                           FBENGINE_GOLDEN_IMAGES="${CMAKE_CURRENT_SOURCE_DIR}/golden"
                           FBENGINE_GOLDEN_FAILURES="${PROJECT_BINARY_DIR}/golden-failures"
                           FBENGINE_GOLDEN_UPDATES="${PROJECT_BINARY_DIR}/golden-updates"
                           FBENGINE_TEST_SCRATCH="${PROJECT_BINARY_DIR}/test-scratch")
fbengine_set_warnings(fbengine_gpu_tests)
fbengine_add_shaders(TARGETS fbengine_gpu_tests NAMESPACE fbe::test_shaders
                     HEADER test_shaders.hpp
                     SHADERS gpu/shaders/triangle.slang gpu/shaders/textured.slang
                             gpu/shaders/shape.slang gpu/shaders/points.slang
                             gpu/shaders/spheres.slang gpu/shaders/poles.slang
                             gpu/shaders/game_shader.slang
                     INCLUDES gpu/shaders/own)
catch_discover_tests(fbengine_gpu_tests TEST_PREFIX "gpu." ADD_TAGS_AS_LABELS
                     DL_PATHS "${fbengine_test_dll_paths}"
                     PROPERTIES ENVIRONMENT "${fbengine_vulkan_environment}" TIMEOUT 300)

# The scene's insides, built in from fbengine_internal: how moves are run, and their scratch.
add_executable(fbengine_scene_internal_tests scene/moves_tests.cpp)
target_link_libraries(fbengine_scene_internal_tests PRIVATE volk Vulkan::Headers
                      Catch2::Catch2WithMain fbengine_internal)
target_include_directories(fbengine_scene_internal_tests PRIVATE
                           "${PROJECT_SOURCE_DIR}/native/fbengine")
fbengine_set_warnings(fbengine_scene_internal_tests)
catch_discover_tests(fbengine_scene_internal_tests TEST_PREFIX "scene.internal."
                     DL_PATHS "${fbengine_test_dll_paths}")

# A golden image update takes only a passing test's images: the hidden test that fails after its
# check, run updating, has to leave its image unwritten.
add_test(NAME gpu.golden_updates_only_from_passing_tests
         COMMAND "${CMAKE_COMMAND}" "-DTESTS=$<TARGET_FILE:fbengine_gpu_tests>"
                 "-DIMAGE=${CMAKE_CURRENT_SOURCE_DIR}/golden/failed_update.png"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/golden/failed_update.cmake")
set_tests_properties(gpu.golden_updates_only_from_passing_tests PROPERTIES
                     LABELS "gpu" TIMEOUT 180
                     ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}")
if(fbengine_test_dll_paths)
  set_property(TEST gpu.golden_updates_only_from_passing_tests PROPERTY ENVIRONMENT_MODIFICATION
               "PATH=path_list_prepend:${fbengine_test_dll_paths}")
endif()

# The packages' own tests, run by `fb test`, and the examples, run by `fb run` and checked against
# what they print, each after the DLLs are built into the packages' native/<target>/ folders.
option(FBENGINE_FB_TESTS "Test the packages and the examples with fb" ON)
if(FBENGINE_FB_TESTS)
  find_program(FBENGINE_FB fb REQUIRED DOC "The fb that builds and tests the packages")

  # Slang's compiler as a test needs it, of the release that's asked for and of another.
  add_executable(fbengine_fake_slangc tools/fake_slangc.cpp)
  target_compile_definitions(fbengine_fake_slangc PRIVATE
                             FBE_SLANG_VERSION="${fbengine_slang_version}")
  add_executable(fbengine_wrong_slangc tools/fake_slangc.cpp)
  target_compile_definitions(fbengine_wrong_slangc PRIVATE FBE_SLANG_VERSION="0.0.1")
  if(MINGW)
    # Run from anywhere, with no DLLs of MinGW's on the PATH.
    target_link_options(fbengine_fake_slangc PRIVATE -static -mcrtdll=ucrt)
    target_link_options(fbengine_wrong_slangc PRIVATE -static -mcrtdll=ucrt)
  endif()

  # Every package's own tests, with SDL's offscreen video, so no test opens a window, as each test
  # asks too.
  file(GLOB fbengine_packages CONFIGURE_DEPENDS LIST_DIRECTORIES true
       "${PROJECT_SOURCE_DIR}/packages/*")
  foreach(package IN LISTS fbengine_packages)
    if(EXISTS "${package}/project.toml")
      cmake_path(GET package FILENAME name)
      add_test(NAME ${name}.package COMMAND "${FBENGINE_FB}" test "${package}")
      set_tests_properties(${name}.package PROPERTIES ENVIRONMENT
                           "FBENGINE_FAKE_SLANGC=$<TARGET_FILE:fbengine_fake_slangc>;FBENGINE_WRONG_SLANGC=$<TARGET_FILE:fbengine_wrong_slangc>;FBENGINE_FAKE_SLANGC_LOG=${CMAKE_CURRENT_BINARY_DIR}/fake_slangc.log;FBENGINE_FAKE_SLANGC_SPV=${CMAKE_CURRENT_BINARY_DIR}/test_shaders/game_shader.spv;FBENGINE_SLANGC=${FBENGINE_SLANGC};SDL_VIDEO_DRIVER=offscreen;FBENGINE_TEST_FONTS=${harfbuzz_SOURCE_DIR}/test/api/fonts;FBENGINE_OWN_TEST_FONTS=${CMAKE_CURRENT_SOURCE_DIR}/fonts;FBENGINE_PACKAGES=${PROJECT_SOURCE_DIR}/packages")
    endif()
  endforeach()

  # A program that ends with the GPU going, each way it can, and has to end: Windows' ending a process
  # with it going seems to have waited for good on CI. An end of its own stops the GPU first, and says
  # so; a panic and Process.Exit end by the runtime's TerminateProcess, past what the engine can hook.
  if(WIN32)
    foreach(case IN ITEMS ends panics exits)
      add_test(NAME gpu.ends.${case}
               COMMAND "${FBENGINE_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/gpu/ends"
                       -- ${case})
      set_tests_properties(gpu.ends.${case} PROPERTIES
                           LABELS "gpu" TIMEOUT 120
                           ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;FBENGINE_SAY_THE_END=1;${fbengine_vulkan_environment}")
    endforeach()
    set_tests_properties(gpu.ends.ends PROPERTIES
                         PASS_REGULAR_EXPRESSION "stopped the GPU as the process ended")
    set_tests_properties(gpu.ends.panics PROPERTIES
                         PASS_REGULAR_EXPRESSION "panic: the program panics")
  endif()

  # E3's arcade example, playing itself headless, the same every time, its pilot's run and a run
  # with no one flying, saving the frames that examples.arcade.golden, a hidden test of
  # fbengine_gpu_tests run after them, compares with their golden images, all on the tests' Vulkan.
  # Its rules' tests; and anything else on its command line says how it's run and fails, with no
  # window.
  add_test(NAME examples.arcade
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/arcade"
                   -- --frames "${PROJECT_BINARY_DIR}/example-frames/arcade")
  set_tests_properties(examples.arcade PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       FIXTURES_SETUP arcade_frames TIMEOUT 300
                       PASS_REGULAR_EXPRESSION "Running arcade[^\n]*\nscore 120, lives 3\r?\n$")
  add_test(NAME examples.arcade.idle
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/arcade"
                   -- --frames "${PROJECT_BINARY_DIR}/example-frames/arcade" --idle)
  set_tests_properties(examples.arcade.idle PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       FIXTURES_SETUP arcade_frames TIMEOUT 300
                       PASS_REGULAR_EXPRESSION "Running arcade[^\n]*\nscore 0, lives 0\r?\n$")
  add_test(NAME examples.arcade.golden
           COMMAND fbengine_gpu_tests "the arcade example's frames match their golden images")
  set_tests_properties(examples.arcade.golden PROPERTIES
                       LABELS "gpu;golden" FIXTURES_REQUIRED arcade_frames
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}")
  if(fbengine_test_dll_paths)
    set_property(TEST examples.arcade.golden PROPERTY ENVIRONMENT_MODIFICATION
                 "PATH=path_list_prepend:${fbengine_test_dll_paths}")
  endif()
  # E7's done-when: Khronos's sample models, skinned and morphed by their files, by numbers against
  # tests/models/reference.py's, and their frames against their golden images.
  add_test(NAME models.sample
           COMMAND "${FBENGINE_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/models"
                   -- --frames "${PROJECT_BINARY_DIR}/example-frames/models")
  set_tests_properties(models.sample PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       FIXTURES_SETUP model_frames TIMEOUT 300
                       PASS_REGULAR_EXPRESSION "
32 of 32 frames as worked out\r?\n$")
  # What masks.bin and worked_out.fb were worked out from, and that it refuses what it can't work out.
  find_program(FBENGINE_PYTHON NAMES python3 python)
  if(FBENGINE_PYTHON)
    add_test(NAME models.reference
             COMMAND "${FBENGINE_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/models/reference.py" --check)
    set_tests_properties(models.reference PROPERTIES PASS_REGULAR_EXPRESSION "as worked out")
    # The export check's macOS side, by stubs of otool and nm, so it's checked without a Mac.
    add_test(NAME tools.check_exports
             COMMAND "${FBENGINE_PYTHON}" "${PROJECT_SOURCE_DIR}/tools/check_exports_test.py")
    set_tests_properties(tools.check_exports PROPERTIES PASS_REGULAR_EXPRESSION "macOS side holds")
    # Each target that builds gltf.cpp reads the simdjson fastgltf fetched, as its own compile says.
    add_test(NAME tools.check_simdjson
             COMMAND "${FBENGINE_PYTHON}" "${PROJECT_SOURCE_DIR}/tools/check_simdjson.py" "${PROJECT_BINARY_DIR}")
    set_tests_properties(tools.check_simdjson PROPERTIES
                         PASS_REGULAR_EXPRESSION "targets read the simdjson fastgltf fetched")
    # Each DLL's notices name every library it links, as the build links it, and that check fails as
    # it should. The build's link graph is CMake's file API's codemodel, which this asks for, and
    # CMake answers as it generates.
    cmake_file_api(QUERY API_VERSION 1 CODEMODEL 2)
    add_test(NAME tools.check_notices
             COMMAND "${FBENGINE_PYTHON}" "${PROJECT_SOURCE_DIR}/tools/check_notices.py" "${PROJECT_BINARY_DIR}"
                     "${PROJECT_SOURCE_DIR}")
    set_tests_properties(tools.check_notices PROPERTIES
                         PASS_REGULAR_EXPRESSION "name every library they link")
    add_test(NAME tools.check_notices_test
             COMMAND "${FBENGINE_PYTHON}" "${PROJECT_SOURCE_DIR}/tools/check_notices_test.py")
    set_tests_properties(tools.check_notices_test PROPERTIES PASS_REGULAR_EXPRESSION "cases hold")
    # Each library's `fb publish --dry-run` passes, in publish.yml's order, with a folder of its own
    # for the registry, so nothing's uploaded.
    add_test(NAME tools.check_publish
             COMMAND "${FBENGINE_PYTHON}" "${PROJECT_SOURCE_DIR}/tools/check_publish.py" "${FBENGINE_FB}"
                     "${PROJECT_SOURCE_DIR}")
    set_tests_properties(tools.check_publish PROPERTIES
                         PASS_REGULAR_EXPRESSION "libraries' dry runs passed, in publish.yml's order"
                         TIMEOUT 900)
    add_test(NAME tools.check_publish_test
             COMMAND "${FBENGINE_PYTHON}" "${PROJECT_SOURCE_DIR}/tools/check_publish_test.py")
    set_tests_properties(tools.check_publish_test PROPERTIES PASS_REGULAR_EXPRESSION "cases hold")
    # The editor packed for this target as publish.yml packs it for the release: its program by
    # this OS's name, its DLLs, and the notices of what fb build bundled and of each DLL's package.
    add_test(NAME tools.pack_editor
             COMMAND "${FBENGINE_PYTHON}" "${PROJECT_SOURCE_DIR}/tools/pack_editor.py" "${FBENGINE_FB}"
                     "${PROJECT_VERSION}" "${FBENGINE_TARGET}" "${CMAKE_CURRENT_BINARY_DIR}/packed_editor"
                     "${PROJECT_SOURCE_DIR}")
    set_tests_properties(tools.pack_editor PROPERTIES
                         PASS_REGULAR_EXPRESSION "packed the editor, its DLLs, and the notices"
                         TIMEOUT 900)
    add_test(NAME tools.pack_editor_test
             COMMAND "${FBENGINE_PYTHON}" "${PROJECT_SOURCE_DIR}/tools/pack_editor_test.py")
    set_tests_properties(tools.pack_editor_test PROPERTIES PASS_REGULAR_EXPRESSION "cases hold")
  endif()
  add_test(NAME models.golden
           COMMAND fbengine_gpu_tests "the sample models' frames match their golden images")
  set_tests_properties(models.golden PROPERTIES
                       LABELS "gpu;golden" FIXTURES_REQUIRED model_frames
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}")
  if(fbengine_test_dll_paths)
    set_property(TEST models.golden PROPERTY ENVIRONMENT_MODIFICATION
                 "PATH=path_list_prepend:${fbengine_test_dll_paths}")
  endif()
  add_test(NAME examples.arcade.rules COMMAND "${FBENGINE_FB}" test "${PROJECT_SOURCE_DIR}/examples/arcade")
  set_tests_properties(examples.arcade.rules PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}")
  # Its --watch, which a person watches in a window (tools/show.py), walked here offscreen, a step
  # each frame: the pilot's run again.
  add_test(NAME examples.arcade.watch
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/arcade"
                   -- --watch --steps 300)
  set_tests_properties(examples.arcade.watch PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       TIMEOUT 300
                       PASS_REGULAR_EXPRESSION "\nwatched 300 steps, score 120, lives 3\r?\n$")
  add_test(NAME examples.arcade.usage
           COMMAND "${CMAKE_COMMAND}" "-DFB=${FBENGINE_FB}" "-DEXAMPLE=${PROJECT_SOURCE_DIR}/examples/arcade"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/arcade_usage.cmake")
  set_tests_properties(examples.arcade.usage PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen"
                       TIMEOUT 600)

  # fb's own lines, building the program, come before what it prints.
  add_test(NAME examples.hello
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/hello")
  set_tests_properties(examples.hello PROPERTIES
                       PASS_REGULAR_EXPRESSION "Running hello[^\n]*\nfbengine ${PROJECT_VERSION}\r?\n$")

  # Input's cases that need a process of their own: what panics, and what's set before the video
  # starts, each checked against what it prints.
  set(fbengine_input_cases
      "key-out-of-range|panic: Simulate[.]KeyDown[(]9999[)]: SDL has no such key"
      "sphere-segments|panic: Sphere: a shape has 3 to 100 segments, not 1 at [^\n]*main[.]fb"
      "button-out-of-range|panic: Simulate[.]MouseDown[(]33[)]: a mouse's buttons are 1 to 32"
      "draws-composition|draws False\r?\ndraws True\r?\ndraws True\r?\npanic: who draws the composition is set before"
      "key-name-before-video|A A\r?\npanic: headless is set before"
      "rumble-negative|panic: Rumble: a rumble lasts 0 seconds or more, not -1"
      "rumble-nan|panic: RumbleTriggers: a rumble lasts 0 seconds or more, not"
      "joystick-sizes|panic: Simulate[.]ConnectJoystick: a joystick has 0 to 255 axes, buttons, and hats, not 0, 256, and 0"
      "pads-before-start|before a Flip True\r?\neach connects at the first True\r?\nand once True"
      "show-message|Missing file: rooms.ini isn't there\r?\nwent on"
      "show-message-offscreen|Missing file: rooms.ini isn't there\r?\nwent on"
      "binding-key-out-of-range|panic: Binding[.]OfKey[(]9999[)]: SDL has no such key"
      "binding-button-out-of-range|panic: Binding[.]OfMouseButton[(]0[)]: a mouse's buttons are 1 to 32"
      "binding-nan-scale|panic: Binding[.]OfGamepadAxis: a scale is a finite number, not"
      "bind-unchecked|panic: Action[.]Bind: a key binding's code is 1 to 511, not 9999"
      "bind-y-infinite|panic: Action[.]BindY: a binding's scale is a finite number, not Inf"
      "replace-overflows|panic: Action[.]Replace: a binding's scale is a finite number, not Inf"
      "mask-threshold|panic: Mask[.]FromImage: a threshold is 1 to 255, not 0"
      "mask-region-outside|panic: Mask[.]FromImage: Rect[(]2, 0, 3, 4[)] isn't whole pixels in the image, 4 by 4"
      "mask-region-fraction|panic: Mask[.]FromImage: Rect[(]0[.]5, 0, 2, 2[)] isn't whole pixels in the image"
      "mask-region-overflowing|panic: Mask[.]FromImage: Rect[(]2147483520, 0, 2147483520, 4[)] isn't whole pixels"
      "mask-pixels-short|panic: Mask[.]FromPixels: 2 by 1 RGBA8 pixels are 8 bytes, not 4"
      "mask-empty-cell|panic: Mask[.]Frames: a cell is 1 pixel or more across and down, not 0 by 4")
  foreach(case IN LISTS fbengine_input_cases)
    string(REPLACE "|" ";" case "${case}")
    list(GET case 0 name)
    list(GET case 1 printed)
    add_test(NAME input.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/tests/input" -- ${name})
    set_tests_properties(input.${name} PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen"
                         PASS_REGULAR_EXPRESSION "${printed}" TIMEOUT 120)
  endforeach()

  add_test(NAME examples.controls
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/controls")
  set_tests_properties(examples.controls PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen"
                       PASS_REGULAR_EXPRESSION "Running controls[^\n]*\nmove [(]0, -1[)], jump True\r?\nmove [(]1, 0[)], jump True\r?\n$")

  # E14's proofs, each of a requirement: what each prints, line by line, and its --watch walked
  # offscreen. One that needs a GPU says so on a machine with none, and is skipped.
  # A --watch, walked offscreen, with what a person would do at its frames, and how it ends.
  function(fbengine_add_watched test folder driving printed)
    add_test(NAME ${test}
             COMMAND "${CMAKE_COMMAND}" "-DFB=${FBENGINE_FB}" "-DFOLDER=${folder}"
                     "-DARGUMENTS=--;--watch;--frames;40;${driving}" -P "${CMAKE_CURRENT_SOURCE_DIR}/proof_exit.cmake")
    set_tests_properties(${test} PROPERTIES
                         FAIL_REGULAR_EXPRESSION "exit code [^0]"
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                         PASS_REGULAR_EXPRESSION "${printed}"
                         SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 120)
  endfunction()
  function(fbengine_add_proof_of_numbers name)
    list(JOIN ARGN "\r?\n" printed)
    add_test(NAME proof.${name}
             COMMAND "${CMAKE_COMMAND}" "-DFB=${FBENGINE_FB}" "-DFOLDER=${PROJECT_SOURCE_DIR}/examples/proof/${name}"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/proof_exit.cmake")
    set_tests_properties(proof.${name} PROPERTIES
                         FAIL_REGULAR_EXPRESSION "exit code [^0]"
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;FBENGINE_OWN_TEST_FONTS=${CMAKE_CURRENT_SOURCE_DIR}/fonts;${fbengine_vulkan_environment}"
                         PASS_REGULAR_EXPRESSION "\n${printed}\r?\n$"
                         SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 120)
  endfunction()
  function(fbengine_add_proof name)
    fbengine_add_proof_of_numbers(${name} ${ARGN})
    set(folder "${PROJECT_SOURCE_DIR}/examples/proof/${name}")
    add_test(NAME proof.${name}_watch
             COMMAND "${CMAKE_COMMAND}" "-DFB=${FBENGINE_FB}" "-DFOLDER=${folder}"
                     "-DARGUMENTS=--;--watch;--frames;40" -P "${CMAKE_CURRENT_SOURCE_DIR}/proof_exit.cmake")
    set_tests_properties(proof.${name}_watch PROPERTIES
                         FAIL_REGULAR_EXPRESSION "exit code [^0]"
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                         PASS_REGULAR_EXPRESSION "\nwatched 40 frames\r?\n$"
                         SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 120)
    fbengine_add_watched(proof.${name}_escape "${folder}" "--press-at;1" "\nwatched 2 frames\r?\n$")
    fbengine_add_watched(proof.${name}_close "${folder}" "--close-at;1" "\nwatched 2 frames\r?\n$")
  endfunction()
  fbengine_add_proof(frame_timing_and_input
                     "after a sleep of 20 ms, the clock's gone 15 or more True"
                     "Flip[(]False[)] waits False"
                     "Flip[(][)] waits True"
                     "frames 3"
                     "10 frames at a limit of 100 took a tenth of a second or more True"
                     "the last one's delta 9 ms or more True"
                     "W down True, hit True"
                     "then down True, hit False"
                     "then down False, up True"
                     "flushed, hit False"
                     "by their numbers: 17 W, 30 A, 31 S, 32 D, 57 SPACE, 42 LEFT_SHIFT, 15 TAB, 29 LEFT_CONTROL, 63 F5, 61 F3"
                     "a number with no key: True"
                     "of 10 binds, held alone 10"
                     "mouse 110, 45, speed 10, -5"
                     "hit True, wheel 2"
                     "a button down True, hit True then False"
                     "then down False"
                     "moved 200, 100, speed 0, 0"
                     "cursor shown False"
                     "relative True, speed 4, -2"
                     "focused False then True")
  fbengine_add_proof(sprites_over_a_scene
                     "the scene's middle is at 160, 90"
                     "the mesh's middle, and off it: [(]120, 130, 150, 255[)], behind"
                     "2D over the mesh, and the mesh beside it: [(]10, 200, 30, 255[)], [(]120, 130, 150, 255[)]"
                     "a frame's block, and what's keyed: [(]255, 200, 80, 255[)], behind"
                     "the last frame's: [(]255, 200, 80, 255[)], behind"
                     "scaled from its handle: [(]255, 200, 80, 255[)], [(]255, 200, 80, 255[)], behind"
                     "turned a quarter: [(]255, 200, 80, 255[)], [(]255, 200, 80, 255[)], behind"
                     "a rect, an oval's middle and corner, a line: [(]200, 40, 40, 255[)], [(]200, 40, 40, 255[)], behind, [(]200, 40, 40, 255[)]"
                     "its width 41[.]46, of \"ScoreScore\" 82[.]91"
                     "its ink's the pen's True, beside it False"
                     "a font of a cell of 20: its ascent and descent 20[.]0"
                     "its ink's the pen's True"
                     "resized, its other key color: [(]50, 90, 200, 255[)], [(]50, 90, 200, 255[)], behind, behind"
                     "its block's corner at its handle: [(]255, 200, 80, 255[)], [(]255, 200, 80, 255[)], behind, behind, behind"
                     "frame 1 of a file: [(]255, 200, 80, 255[)], behind, behind"
                     "rects True, touching False"
                     "each frame's solid pixels 4, 16"
                     "a block on a block True, off it False"
                     "bounds apart by the key color: overlap True, collide False"
                     "a rect on its key color False, on its block True")
  fbengine_add_proof(pixels_of_images_and_textures
                     "the image's red adds up to 30720"
                     "a pixel from the GPU [(]80, 144, 0, 255[)]"
                     "locked 32 by 32"
                     "as written [(]24, 32, 128, 255[)]"
                     "unlocked, from the GPU [(]24, 32, 128, 255[)]"
                     "a plain texture's alpha from the GPU [(]255, 255, 255, 128[)]"
                     "drawn over black [(]0, 0, 0, 255[)], [(]137, 137, 137, 255[)], [(]188, 188, 188, 255[)], [(]255, 255, 255, 255[)]"
                     "drawn, then read [(]255, 0, 0, 255[)]"
                     "a part of it 4 by 4: [(]255, 0, 0, 255[)], [(]104, 104, 128, 255[)]"
                     "the screen's [(]1, 2, 3, 255[)]"
                     "a million written in under 10 seconds True")
  fbengine_add_proof(display_modes
                     "there's a display True"
                     "its desktop's a size True"
                     "its modes for fullscreen 0"
                     "windowed at first WINDOWED, 640 by 360"
                     "asked for BORDERLESS: BORDERLESS, 1024 by 768, the desktop's True"
                     "asked for WINDOWED: WINDOWED, 640 by 360, the desktop's False"
                     "asked for FULLSCREEN: FULLSCREEN, 1024 by 768, the desktop's True"
                     "asked for WINDOWED: WINDOWED, 640 by 360, the desktop's False"
                     "the screen's 320 by 180, its own True"
                     "shown in 640 by 360 of the window"
                     "the window's again 640 by 360, True")
  # A real display's modes, and a switch to one, which opens a window, so it's only ever made where
  # FBENGINE_REAL_DISPLAY is set, as CI's runners do, and never on a person's machine; without it, the
  # program refuses, opening nothing.
  add_test(NAME proof.display_modes_refuses
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/proof/display_modes"
                   -- --real-display)
  set_tests_properties(proof.display_modes_refuses PROPERTIES TIMEOUT 300
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;FBENGINE_REAL_DISPLAY="
                       PASS_REGULAR_EXPRESSION "--real-display opens a window: it's for CI")
  if(DEFINED ENV{FBENGINE_REAL_DISPLAY} AND "$ENV{FBENGINE_REAL_DISPLAY}" STREQUAL "1")
    add_test(NAME proof.display_modes_real
             COMMAND "${FBENGINE_FB}" --color never run
                     "${PROJECT_SOURCE_DIR}/examples/proof/display_modes" -- --real-display)
    set_tests_properties(proof.display_modes_real PROPERTIES TIMEOUT 300
                         ENVIRONMENT "FBENGINE_REAL_DISPLAY=1;${fbengine_vulkan_environment}"
                         PASS_REGULAR_EXPRESSION "the real display's modes are real True")
  endif()
  fbengine_add_proof(text_input
                     "off, typed \"\""
                     "on, typed \"a\", in order TEXT, PRESS"
                     "text input off without a field True"
                     "a field's focus turns it on True"
                     "the name \"Cabin 7 save\", changed True"
                     "edited \"ACabin 7 sav\", cursor 1"
                     "\"e\" and its accent are 1 long"
                     "the line has the focus True"
                     "submitted True: \"noclip on\""
                     "no focus, text input False")
  fbengine_add_proof(debug_ui_and_console
                     "rows [[]frame, speed, noclip, respawn[]]"
                     "a watched row's the frame's True, then the next's True"
                     "noclip True, rows [[]frame, speed, noclip, respawn, only with noclip[]]"
                     "noclip False, rows [[]frame, speed, noclip, respawn[]]"
                     "respawns 1"
                     "speed by End 20"
                     "speed by command 7"
                     "finished \"speed \""
                     "recalled \"nosuch\""
                     "printed [[]> speed 7, speed is 7, > fail, fail: it can't be done, > nosuch, there's no command nosuch. help lists them[]]"
                     "run [[]speed 7, fail, nosuch[]]"
                     "closed True, focus none"
                     "the game's band beside the panel [(]76, 141, 255, 255[)]"
                     "the panel over it True, [(]28, 31, 36, 255[)]")

  # A --watch's own keys, each pressed by the walk.
  set_tests_properties(proof.frame_timing_and_input_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nby its keys: relative True, shown False, vsync False, moved True, flushed True\r?\nwatched 40 frames\r?\n$")
  # Escape with the console open closes it, and a second ends the program.
  fbengine_add_watched(proof.debug_ui_and_console_escape_console
                       "${PROJECT_SOURCE_DIR}/examples/proof/debug_ui_and_console"
                       "--press-at;4;--press-at;6" "\nwatched 7 frames\r?\n$")
  # With no GPU, those that need one to make what they draw say so, and the rest draw nothing.
  fbengine_add_proof(textures_by_their_settings
                     "a [.]cwm's sound of 4800 frames"
                     "by default 5 levels, RGBA8, REPEAT"
                     "mipmaps = false, 1 level"
                     "srgb = false, RGBA8_LINEAR"
                     "alpha in its pixels True"
                     "alpha in a whole one False"
                     "clamped CLAMP, CLAMP"
                     "clamped across alone CLAMP, REPEAT"
                     "clamped down alone REPEAT, CLAMP"
                     "across alone, past its right green, past its bottom red"
                     "down alone, past its right red, past its bottom blue"
                     "both, past its right green, past its bottom blue"
                     "neither, past its right red, past its bottom red"
                     "a strip of 4 frames"
                     "frame 0 [(]255, 0, 0, 255[)]"
                     "frame 1 [(]0, 255, 0, 255[)]"
                     "frame 2 [(]0, 0, 255, 255[)]"
                     "frame 3 [(]255, 255, 255, 255[)]"
                     "on a 3D material's layer, frame 0 red"
                     "on a 3D material's layer, frame 1 green"
                     "on a 3D material's layer, frame 2 blue"
                     "on a 3D material's layer, frame 3 white"
                     "a JPEG 8 across, its red over 250 True"
                     "a BMP [(]255, 128, 0, 255[)]"
                     "a [.]pt [(]255, 0, 255, 255[)], by a path of backslashes"
                     "a [.]it 4 across"
                     "a DDS: INVALID_DATA, says DDS True"
                     "cooked platform, red True, 5 levels"
                     "cooked uastc, red True, 5 levels"
                     "cooked none, red True, 5 levels"
                     "cooked bc7: sampled or said so True"
                     "cooked astc: sampled or said so True"
                     "a cooked DDS is what it was True")
  # Slack Water, by key presses, offscreen, to each of its endings.
  foreach(name IN ITEMS bright dim dark)
    add_test(NAME examples.slack_water.walk_${name}
             COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/slack_water"
                     -- --walk ${name}
                     "${PROJECT_SOURCE_DIR}/examples/slack_water/tests/walk_${name}.txt")
    set_tests_properties(examples.slack_water.walk_${name} PROPERTIES RESOURCE_LOCK slack_water
                         ENVIRONMENT "SLACK_WATER_CHECKS=1;SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                         TIMEOUT 300)
  endforeach()
  # And on the island, walked in 3D: to each ending, then the road as it floods.
  foreach(name IN ITEMS bright dim dark cut swept wing far_end)
    add_test(NAME examples.slack_water.walk3d_${name}
             COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/slack_water"
                     -- --walk3d ${name}
                     "${PROJECT_SOURCE_DIR}/examples/slack_water/tests/walk3d_${name}.txt")
    set_tests_properties(examples.slack_water.walk3d_${name} PROPERTIES RESOURCE_LOCK slack_water
                         ENVIRONMENT "SLACK_WATER_CHECKS=1;SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                         SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 300)
  endforeach()
  add_test(NAME examples.slack_water.end_screen
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/slack_water"
                   -- --end "${PROJECT_BINARY_DIR}/example-frames/slack_water")
  set_tests_properties(examples.slack_water.end_screen PROPERTIES RESOURCE_LOCK slack_water
                       ENVIRONMENT "SLACK_WATER_CHECKS=1;SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 300)
  # As it ships: a release, from its cooked pak, into a folder this clears, as the game never does.
  set(slack_water_cooked "${PROJECT_BINARY_DIR}/example-frames/slack_water_cooked")
  add_test(NAME examples.slack_water.cooked_clean
           COMMAND "${CMAKE_COMMAND}" -E rm -rf "${slack_water_cooked}")
  set_tests_properties(examples.slack_water.cooked_clean PROPERTIES RESOURCE_LOCK slack_water
                       FIXTURES_SETUP slack_water_cooked_clean)
  add_test(NAME examples.slack_water.cooked
           COMMAND "${FBENGINE_FB}" --color never run --release
                   "${PROJECT_SOURCE_DIR}/examples/slack_water"
                   -- --cooked "${PROJECT_SOURCE_DIR}/examples/slack_water/tests/walk3d_bright.txt"
                   "${slack_water_cooked}")
  set_tests_properties(examples.slack_water.cooked PROPERTIES RESOURCE_LOCK slack_water
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       FIXTURES_REQUIRED slack_water_cooked_clean
                       FIXTURES_SETUP slack_water_cooked_views
                       SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 600)
  # What it draws from its pak is close to what it draws from its files.
  add_test(NAME examples.slack_water.cooked_alike
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/slack_water"
                   -- --alike "${PROJECT_BINARY_DIR}/example-frames/slack_water"
                   "${slack_water_cooked}" 25)
  set_tests_properties(examples.slack_water.cooked_alike PROPERTIES RESOURCE_LOCK slack_water
                       ENVIRONMENT "SLACK_WATER_CHECKS=1;SDL_VIDEO_DRIVER=offscreen"
                       FIXTURES_REQUIRED "slack_water_views;slack_water_cooked_views"
                       TIMEOUT 300)
  # Which is to say something: not from a pak, or against another walk's words, it says so.
  add_test(NAME examples.slack_water.cooked_not
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/slack_water"
                   -- --cooked "${PROJECT_SOURCE_DIR}/examples/slack_water/tests/walk3d_bright.txt"
                   "${PROJECT_BINARY_DIR}/example-frames/slack_water_cooked_not")
  add_test(NAME examples.slack_water.cooked_other_walk
           COMMAND "${FBENGINE_FB}" --color never run --release
                   "${PROJECT_SOURCE_DIR}/examples/slack_water"
                   -- --cooked "${PROJECT_SOURCE_DIR}/examples/slack_water/tests/walk3d_dark.txt"
                   "${PROJECT_BINARY_DIR}/example-frames/slack_water_cooked_other")
  set_tests_properties(examples.slack_water.cooked_not examples.slack_water.cooked_other_walk
                       PROPERTIES RESOURCE_LOCK slack_water
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 600)
  set_tests_properties(examples.slack_water.cooked_not PROPERTIES
                       PASS_REGULAR_EXPRESSION "it's from its pak, cooked False")
  set_tests_properties(examples.slack_water.cooked_other_walk PROPERTIES
                       PASS_REGULAR_EXPRESSION "what the walk said is what's kept False")
  # And as a player has it: its executable and pak in a folder of their own, with nothing else left;
  # then what a player sees of a pak that's missing, broken, or another version's, which is a word,
  # with no window, and on a Mac, as a bundle.
  function(slack_water_shipped name)
    add_test(NAME examples.slack_water.${name}
             COMMAND "${CMAKE_COMMAND}" "-DFB=${FBENGINE_FB}"
                     "-DSOURCE=${PROJECT_SOURCE_DIR}/examples/slack_water"
                     "-DPACKAGES=${PROJECT_SOURCE_DIR}/packages"
                     "-DWORK=${PROJECT_BINARY_DIR}/example-frames/slack_water_${name}"
                     "-DSUFFIX=${CMAKE_EXECUTABLE_SUFFIX}" ${ARGN}
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/slack_water_shipped.cmake")
    set_tests_properties(examples.slack_water.${name} PROPERTIES RESOURCE_LOCK slack_water
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                         SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 600)
  endfunction()
  slack_water_shipped(shipped)
  slack_water_shipped(shipped_no_pak -DPAK=MISSING)
  set_tests_properties(examples.slack_water.shipped_no_pak PROPERTIES PASS_REGULAR_EXPRESSION
                       "can't read its files: there's no [^\n]*slack_water[.]pak")
  slack_water_shipped(shipped_broken_pak -DPAK=BROKEN)
  set_tests_properties(examples.slack_water.shipped_broken_pak PROPERTIES PASS_REGULAR_EXPRESSION
                       "can't read its files: [^\n]*slack_water[.]pak")
  slack_water_shipped(shipped_old_pak -DPAK=OLD)
  set_tests_properties(examples.slack_water.shipped_old_pak PROPERTIES PASS_REGULAR_EXPRESSION
                       "can't read its files: [^\n]*is of 0[.]0[.]1, and this game's of ")
  if(APPLE)
    slack_water_shipped(shipped_bundle -DBUNDLE=ON)
  endif()
  # What a frame costs, as the program's own timing says; on a software GPU it's no more than that it
  # runs. The program refuses a real GPU unless SLACK_WATER_REAL_GPU=1 and it's the only device shown.
  # A Mac's Vulkan is MoltenVK, on its real GPU, its only one, so there that's what this measures, and
  # nowhere else does anything here set it.
  add_test(NAME examples.slack_water.frame_cost
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/slack_water"
                   -- --fps 0 15)
  set_tests_properties(examples.slack_water.frame_cost PROPERTIES RESOURCE_LOCK slack_water
                       ENVIRONMENT "SLACK_WATER_CHECKS=1;SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "the GPU's not lost, and its time was read True"
                       SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 300)
  if(APPLE)
    set_property(TEST examples.slack_water.frame_cost APPEND PROPERTY ENVIRONMENT "SLACK_WATER_REAL_GPU=1")
  endif()
  # Each mode that draws with no person there refuses when the loader shows several devices: it would pick
  # the best for itself, on a machine it could take down.
  foreach(mode_and_arguments "cooked|--cooked|a|b" "walk3d|--walk3d|bright" "end|--end|a" "hear|--hear"
          "feel|--feel" "frames|--frames|a")
    string(REPLACE "|" ";" mode_arguments "${mode_and_arguments}")
    list(POP_FRONT mode_arguments mode_name)
    add_test(NAME examples.slack_water.refuses_${mode_name}
             COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/slack_water"
                     -- ${mode_arguments})
    set_tests_properties(examples.slack_water.refuses_${mode_name} PROPERTIES RESOURCE_LOCK slack_water
                         ENVIRONMENT "SLACK_WATER_CHECKS=1;SLACK_WATER_DEVICES=2;SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                         PASS_REGULAR_EXPRESSION "this draws only when the loader shows one device"
                         SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 300)
  endforeach()
  add_test(NAME examples.slack_water.hear
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/slack_water"
                   -- --hear)
  set_tests_properties(examples.slack_water.hear PROPERTIES RESOURCE_LOCK slack_water
                       ENVIRONMENT "SLACK_WATER_CHECKS=1;SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 300)
  add_test(NAME examples.slack_water.feel
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/slack_water"
                   -- --feel)
  set_tests_properties(examples.slack_water.feel PROPERTIES RESOURCE_LOCK slack_water
                       ENVIRONMENT "SLACK_WATER_CHECKS=1;SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 300)
  # Its flow, and its tools' sounds and materials, by their numbers, with nothing drawn.
  add_test(NAME examples.slack_water.rules
           COMMAND "${FBENGINE_FB}" --color never test "${PROJECT_SOURCE_DIR}/examples/slack_water")
  set_tests_properties(examples.slack_water.rules PROPERTIES RESOURCE_LOCK slack_water TIMEOUT 300)
  add_test(NAME examples.slack_water.tools_rules
           COMMAND "${FBENGINE_FB}" --color never test
                   "${PROJECT_SOURCE_DIR}/examples/slack_water/tools")
  set_tests_properties(examples.slack_water.tools_rules PROPERTIES RESOURCE_LOCK slack_water
                       TIMEOUT 300)
  # And to its best ending, saving what it looks like for a person to see, in its frames' folder,
  # which it fails by if what's drawn isn't what the tide says.
  add_test(NAME examples.slack_water.frames
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/slack_water"
                   -- --frames "${PROJECT_BINARY_DIR}/example-frames/slack_water")
  set_tests_properties(examples.slack_water.frames PROPERTIES RESOURCE_LOCK slack_water
                       ENVIRONMENT "SLACK_WATER_CHECKS=1;SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 300
                       FIXTURES_SETUP slack_water_views)
  # Its assets: what its tools make, here, and what's kept in the repository, each loaded as the game
  # loads them, and the two the same.
  add_test(NAME examples.slack_water.tools
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/slack_water/tools"
                   -- "${PROJECT_BINARY_DIR}/example-frames/slack_water_made")
  set_tests_properties(examples.slack_water.tools PROPERTIES
                       FIXTURES_SETUP slack_water_made TIMEOUT 300
                       PASS_REGULAR_EXPRESSION "18 textures\r?\n13 sounds\r?\n8 joints in Pip\r?\n$")
  foreach(checked IN ITEMS "made=${PROJECT_BINARY_DIR}/example-frames/slack_water_made"
                           "kept=${PROJECT_SOURCE_DIR}/examples/slack_water/assets")
    string(FIND "${checked}" "=" split)
    string(SUBSTRING "${checked}" 0 ${split} name)
    math(EXPR split "${split} + 1")
    string(SUBSTRING "${checked}" ${split} -1 folder)
    add_test(NAME examples.slack_water.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/slack_water"
                     -- --check "${folder}")
    set_tests_properties(examples.slack_water.${name} PROPERTIES RESOURCE_LOCK slack_water
                         FIXTURES_REQUIRED slack_water_made
                         ENVIRONMENT "SLACK_WATER_CHECKS=1;SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                         SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 300)
  endforeach()
  add_test(NAME examples.slack_water.assets_match
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/slack_water"
                   -- --match "${PROJECT_BINARY_DIR}/example-frames/slack_water_made"
                   "${PROJECT_SOURCE_DIR}/examples/slack_water/assets")
  set_tests_properties(examples.slack_water.assets_match PROPERTIES RESOURCE_LOCK slack_water
                       FIXTURES_REQUIRED slack_water_made
                       ENVIRONMENT "SLACK_WATER_CHECKS=1;SDL_VIDEO_DRIVER=offscreen" TIMEOUT 300)
  # The sea's shader is cooked by the game's build step, which every one of its tests runs, by the
  # Slang release the engine's own are.
  get_directory_property(fbengine_tests_here TESTS)
  foreach(test IN LISTS fbengine_tests_here)
    if(test MATCHES "^examples[.]slack_water[.]")
      set_property(TEST "${test}" APPEND PROPERTY ENVIRONMENT "FBENGINE_SLANGC=${FBENGINE_SLANGC}")
    endif()
  endforeach()
  foreach(name IN ITEMS sprites_over_a_scene pixels_of_images_and_textures
          textures_by_their_settings)
    add_test(NAME proof.${name}_no_gpu
             COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/proof/${name}"
                     -- --watch --frames 2)
    set_tests_properties(proof.${name}_no_gpu PROPERTIES
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;FBENGINE_NO_GPU=1"
                         PASS_REGULAR_EXPRESSION "\nno GPU here\r?\n$" TIMEOUT 120)
  endforeach()

  # E6's materials with their own shaders, its build step compiling them with the pinned slangc.
  set(fbengine_own_shaders "${PROJECT_SOURCE_DIR}/examples/proof/materials_with_their_own_shaders")
  # Linear 0.5 is 187.5 in sRGB's bytes, either side of which is right; 0.25 is 137 and 0.125 is 99.
  set(fbengine_own_shader_proof
      "the scene's own: 137, 18[78], 255"
      "a shader of its own: 255, 18[78], 137"
      "its parameter: \\(1, 0[.]5, 0[.]25, 1\\), its shader's the one given True"
      "under half of a black fog: 18[78], 13[67], 9[89]"
      "whatever its shading: 255, 18[78], 137"
      "its UVs, a half each: 18[789], 18[789], 0"
      "its normal, toward the camera: 18[78], 18[78], 255"
      "its vertices' color, a half each: 18[78], 18[78], 18[78]"
      "what it would have drawn, with no layer: 137, 18[78], 255"
      "its layer's texture, a half each: 18[789], 18[789], 18[789]"
      "the light on it, the ambient's 0[.]25 each: 137, 137, 137"
      "its alpha, below a cutoff: 0, 0, 0"
      "and above one: 255, 255, 255"
      "its vertices moved away: 0, 0, 0"
      "and not: 137, 137, 137"
      "with none again: 137, 18[78], 255, its shader none"
      "nothing on the sun's way: True"
      "its shadow, moved onto it: False"
      "and away: True"
      "shaders done")
  list(JOIN fbengine_own_shader_proof "\r?\n" fbengine_own_shader_lines)
  set(fbengine_own_shader_lines "\n${fbengine_own_shader_lines}\r?\n$")
  add_test(NAME proof.materials_with_their_own_shaders
           COMMAND "${FBENGINE_FB}" --color never run "${fbengine_own_shaders}")
  set_tests_properties(proof.materials_with_their_own_shaders PROPERTIES TIMEOUT 300
                       RESOURCE_LOCK fbengine_own_shaders
                       ENVIRONMENT "FBENGINE_SLANGC=${FBENGINE_SLANGC};SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "${fbengine_own_shader_lines}")
  add_test(NAME proof.materials_with_their_own_shaders_watch
           COMMAND "${FBENGINE_FB}" --color never run "${fbengine_own_shaders}" -- --watch --frames 40)
  set_tests_properties(proof.materials_with_their_own_shaders_watch PROPERTIES TIMEOUT 300
                       RESOURCE_LOCK fbengine_own_shaders
                       ENVIRONMENT "FBENGINE_SLANGC=${FBENGINE_SLANGC};SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "\nits water bluest True, its ripple moved True\r?\nwatched 40 frames\r?\n$")
  # Section 6's item 14, its receiving side: what a game gets of a converter's glTF.
  fbengine_add_proof_of_numbers(models_as_a_converter_writes_them
      "nodes under the figure: Bip01, Bip01 Spine, Bip01 Head, Bip01 HeadNub, body#1"
      "joints of its skin: Bip01, Bip01 Spine, Bip01 Head"
      "its clip: walk, at 30 frames a second, 24 frames long"
      "at frame 6 its spine's at 0.625, its head's turned 45"
      "at frame 12 its spine's at 0.75, its head's turned 90"
      "at frame 24 its spine's at 0.5, its head's turned 0"
      "surface 0: brush brick, reading GFX\\\\brick.bmp, 2 by 2"
      "surface 1: brush skin, reading skin_01.png, 2 by 2"
      "surface 0's vertex colors: 1 0 0 1, 0 1 0 0.5, 0 0 1 1, 0.25 0.5 0.75 1"
      "surface 1's vertex colors: 1 0 0 1, 0 0.2 0.4 1, 0 0 1 1, 1 1 1 1")
  # Those that draw: what each prints, and its --watch, which has to have drawn something.
  fbengine_add_proof(swept_collisions
                           "a ball falls 100 and rests at 0[.]00, 0[.]50, 0[.]00"
                           "it touched 1, the floor True, facing 0[.]00, 1[.]00, 0[.]00, at 0[.]00, 0[.]00, 0[.]00"
                           "what it hit has it too True"
                           "a tall one rests at 4[.]00, 1[.]50, 4[.]00"
                           "sliding along a wall ends at 3[.]00, 1[.]00, -4[.]00"
                           "touching the wall True, facing 0[.]00, 0[.]00, 1[.]00"
                           "stopping at it ends at 1[.]20, 1[.]00, -4[.]00"
                           "with none it ends at 0[.]00, 1[.]00, -10[.]00, touching 0"
                           "on a slope, sliding moves it across True, standing True"
                           "over a step of 0[.]3 it reaches z -3[.]00, y 0[.]80"
                           "with a step height of 0[.]1 it stops at z -1[.]55"
                           "a ball into a rock stops at z -3[.]50, y 5[.]00, having hit it True"
                           "iterations by default 8"
                           "SLIDE_HORIZONTAL along a wall: 3[.]000, 1[.]000, -3[.]999"
                           "into a corner: 3[.]999, 1[.]000, -3[.]999, touching 2"
                           "  a contact, facing 0[.]000, 0[.]000, 1[.]000, at 4[.]000, 1[.]000, -4[.]500"
                           "  a contact, facing -1[.]000, 0[.]000, 0[.]000, at 4[.]500, 1[.]000, -3[.]999"
                           "a tall one into a wall: 0[.]000, 2[.]000, -3[.]999, facing 0[.]000, 0[.]000, 1[.]000, at 0[.]000, 2[.]000, -4[.]500"
                           "past a rock to a wall: 2[.]988, 1[.]000, -2[.]964, touching 1"
                           "  by group: level False, other True, first's normal 0[.]333, 0[.]000, 0[.]943"
                           "a tall one onto a tall rock: 0[.]000, 6[.]503, 10[.]000"
                           "around a ring, 8 iterations: 45[.]443, 1[.]000, -4[.]499, touching 7"
                           "around a ring, 10 iterations: 45[.]443, 1[.]000, -4[.]499, touching 7"
                           "around a tight ring, 8 iterations: 74[.]0[0-9][0-9], 1[.]000, -2[.]0[0-9][0-9], touching 8"
                           "around a tight ring, 10 iterations: 73[.]8[0-9][0-9], 1[.]000, -2[.]4[0-9][0-9], touching 10"
                           "a mover into one moving away: -40[.]000, 1[.]000, -1[.]999, the other -40[.]000, 1[.]000, -13[.]000"
                           "a tall one onto a slope of 30: facing -0[.]500, 0[.]866, 0[.]000"
                           "a wall, then a rock: touching 2, level True, other True"
                           "a door moved across the move, this frame: 0[.]000, 1[.]000, -3[.]999"
                           "by UpdateWorld: 0[.]000, 0[.]501, 0[.]000, touching 1"
                           "into a box: 138[.]499, 1[.]000, 0[.]000")
  fbengine_add_proof(picks
                           "down onto the floor: floor at 1[.]00, 0[.]00, 3[.]00, facing 0[.]00, 1[.]00, 0[.]00, 0[.]50 along"
                           "down onto the ball: ball at 2[.]00, 1[.]50, 0[.]00, facing 0[.]00, 1[.]00, 0[.]00, 0[.]35 along"
                           "down onto the crate: crate at -3[.]00, 2[.]00, 0[.]00, facing 0[.]00, 1[.]00, 0[.]00, 0[.]30 along"
                           "the floor's surface True, a triangle True"
                           "the ball's surface True"
                           "through what isn't pickable: floor at 0[.]00, 0[.]00, 3[.]00, facing 0[.]00, 1[.]00, 0[.]00, 0[.]50 along"
                           "from under the floor: nothing"
                           "ignoring the ball: floor at 2[.]00, 0[.]00, 0[.]00, facing 0[.]00, 1[.]00, 0[.]00, 0[.]50 along"
                           "with the ball hidden: floor at 2[.]00, 0[.]00, 0[.]00, facing 0[.]00, 1[.]00, 0[.]00, 0[.]50 along"
                           "a line beside the ball: nothing"
                           "a line of radius 0[.]5 beside it: ball at 2[.]40, 1[.]30, 0[.]00, facing 0[.]80, 0[.]60, 0[.]00, 0[.]97 along"
                           "ahead of something: ball at 2[.]00, 1[.]00, 0[.]50, facing 0[.]00, 0[.]00, 1[.]00, 0[.]45 along"
                           "ahead, not far enough: nothing"
                           "the camera's middle: ball at 2[.]00, 1[.]00, 0[.]50, facing 0[.]00, 0[.]00, 1[.]00, 0[.]01 along"
                           "high in its view: nothing"
                           "a sphere swept onto a mesh: floor at 1[.]000, 0[.]000, 3[.]000, facing 0[.]000, 1[.]000, 0[.]000, 0[.]450 along"
                           "a tall ellipsoid from above: tall at 2[.]000, 5[.]000, 0[.]000, facing 0[.]000, 1[.]000, 0[.]000, 0[.]500 along"
                           "a turned mesh: leaning at -30[.]000, 0[.]000, 0[.]000, facing -0[.]500, 0[.]866, 0[.]000, 0[.]500 along"
                           "ahead of something pickable, from outside its own reach: nothing"
                           "a camera's middle, looking down: floor at 0[.]000, 0[.]000, 0[.]000, facing 0[.]000, 1[.]000, 0[.]000, 0[.]003 along"
                           "off to its right: floor at 1[.]886, 0[.]000, 0[.]000, facing 0[.]000, 1[.]000, 0[.]000, 0[.]003 along"
                           "off to its left: floor at -1[.]886, 0[.]000, 0[.]000, facing 0[.]000, 1[.]000, 0[.]000, 0[.]003 along"
                           "ahead of a mesh with its geometry ahead: nothing"
                           "the floor's triangle there 30")
  fbengine_add_proof(in_view_by_bounds
                           "in front True, behind False"
                           "at its near end True, nearer False"
                           "at its far end True, farther False"
                           "past its top False, inside it True"
                           "a box inside True"
                           "a box half out True"
                           "a box out False"
                           "its bounds are 8 to 28 across"
                           "a long mesh with only its end in view True, its middle False"
                           "moved out False"
                           "a pivot in view True"
                           "behind a wall True"
                           "a line of sight through the wall False"
                           "with it not blocking True"
                           "left False True, right False True"
                           "bottom False True, top False True"
                           "a box with every corner outside, around the view: True"
                           "a mesh with every corner outside, around the view: True"
                           "a turned bar, its corners outside: False, the box around it True"
                           "moved, what it faced False, what it faces now True"
                           "turned about, what it faced False, behind it True"
                           "a pivot out of view False"
                           "a pivot past the far plane False"
                           "a mesh past the far plane False"
                           "a mesh across the far plane True"
                           "a mesh hidden in view True"
                           "through a wall False, a point False, this side of it True"
                           "the wall hidden True"
                           "a target that blocks sight, before the wall True"
                           "past an ellipsoid told to block sight False"
                           "past a box told to block sight False"
                           "at zoom 1, x 9[.]9 True, 10[.]1 False"
                           "y 7[.]4 True, 7[.]6 False"
                           "at zoom 1[.]1, 84[.]5 degrees across"
                           "x 9 True, 9[.]2 False")
  fbengine_add_proof(scene_trees
                           "the room has 2, first door, then lamp"
                           "the handle's parent door, the room's none"
                           "found under the room True, not there True"
                           "the handle's own place 0[.]00, 1[.]00, 0[.]00, in the world 12[.]00, 1[.]00, 0[.]00"
                           "the room turned a quarter: 10[.]00, 1[.]00, -2[.]00"
                           "scaled twice: 14[.]00, 2[.]00, 0[.]00"
                           "under the lamp, world kept: 12[.]00, 1[.]00, 0[.]00, own 2[.]00, 1[.]00, 0[.]00"
                           "back, own kept: 2[.]00, 1[.]00, 0[.]00, world 14[.]00, 1[.]00, 0[.]00"
                           "moved forward, turned a quarter: -1[.]00, 0[.]00, 0[.]00"
                           "translated, whichever way it faces: 0[.]00, 0[.]00, -1[.]00"
                           "pointed at [+]Z, forward is -?0[.]00, -?0[.]00, 1[.]00"
                           "distance to the door 12[.]00"
                           "a copy has 2, its handle True, not the same one True"
                           "in its order: door, lamp"
                           "its handle in the world -8[.]00, 1[.]00, 0[.]00, the first's 12[.]00, 1[.]00, 0[.]00"
                           "room hidden: the room's showing False, the lamp's False, hidden itself False"
                           "room shown: the lamp's True, the door's still False"
                           "door shown: the handle's True, the copy's door was never hidden True"
                           "freed: the room True, its handle True, the copy's False"
                           "TransformPoint: 8[.]000, 1[.]000, 0[.]000, into the parent 0[.]000, 1[.]000, -2[.]000"
                           "TransformVector: -1[.]000, 0[.]000, 0[.]000"
                           "YawTo a place to its left 1[.]571, PitchTo above 0[.]785"
                           "YawOf 1[.]571, PitchOf 0[.]785"
                           "AlignTo: forward 1[.]000, 0[.]000, 0[.]000"
                           "Turn in the world: forward -1[.]000, 0[.]000, 0[.]000"
                           "a copy's door: kind MESH, group 2, pick MESH, blocks True, bounds 1[.]000, 1[.]000, 1[.]000"
                           "its lamp hidden True"
                           "alpha 0: showing True, picked the door"
                           "alpha 0: a mover rests at 0[.]000, 1[.]501, 0[.]000"
                           "alpha 1: what's over it [(]255, 0, 0, 255[)]"
                           "alpha 0: what's over it [(]0, 0, 255, 255[)]")
  fbengine_add_proof(meshes_in_code
                           "1 surface, 4 vertices, 2 triangles"
                           "its second UVs [(]0[.]5, 0[.]75[)], first [(]0, 1[)]"
                           "its color's alpha 0[.]5"
                           "made again 0[.]00, 1[.]00, 0[.]00"
                           "one corner lifted, it leans True"
                           "bounds 0[.]00, 0[.]00, -2[.]00 to 2[.]00, 0[.]00, 0[.]00"
                           "moved: 1[.]00, 0[.]00, -2[.]00"
                           "scaled: 6[.]00, 0[.]00, 0[.]00"
                           "turned, it's 2[.]00 across"
                           "fitted: -1[.]00, 0[.]00, -1[.]00 to 1[.]00, 0[.]00, 1[.]00"
                           "picked from above True, flipped False, from below True"
                           "a cube's 24 vertices, joined 2 surfaces"
                           "the clone moved, the first didn't True"
                           "its cull box 100, bounds 1"
                           "a vertex's whole color [(]0, 1, 0, 0[.]5[)]"
                           "VertexPosition 0[.]000, 0[.]000, -2[.]000"
                           "SetVertexNormal 1[.]000, 0[.]000, 0[.]000"
                           "turned a quarter: -2[.]000, 0[.]000, -?0[.]000"
                           "flipped, a normal -?0[.]000, -1[.]000, -?0[.]000, was up"
                           "a clone's vertex -2[.]000, 0[.]000, -?0[.]000, 0[.]000, 1[.]000, 0[.]000, [(]0, 1[)], [(]0[.]5, 0[.]75[)], [(]0, 1, 0, 0[.]5[)]"
                           "joined: its last surface has 3 vertices, 1 triangles, -2[.]000, 0[.]000, -?0[.]000, 0[.]000, 1[.]000, 0[.]000, [(]0, 1[)], [(]0[.]5, 0[.]75[)], [(]0, 1, 0, 0[.]5[)]"
                           "added by a matrix: 5[.]000, 0[.]000, 0[.]000"
                           "a surface's material read back True"
                           "in view [(]255, 0, 0, 255[)]"
                           "its cull box out of view [(]0, 0, 255, 255[)]"
                           "in its surface's material [(]10, 200, 30, 255[)]"
                           "its texture's from wall[.]png True")
  fbengine_add_proof(binary_saves_and_maps
                           "the file's 75 bytes"
                           "its magic MAP1"
                           "seed 1234567890123, time 61[.]5"
                           "2 rooms"
                           "lobby at 0, 0, light 0[.]75, doors 3, locked False"
                           "tunnel, .+ at -8, 16, light 0[.]25, doors 2, locked True"
                           "reading past the end throws True"
                           "its first bytes 77, 65, 80, 49"
                           "the seed's low byte 203, as 1234567890123 % 256 is 203"
                           "an Int32 305419896, a Float32 1"
                           "a name of 2 bytes is an accented e True"
                           "in its folder: options[.]bin, slot 1/"
                           "before deleting, it's there True"
                           "a folder as a file False, a file as a folder False, the folder True"
                           "deleted, it's there False"
                           "its folder removed, it's there False")
  fbengine_add_proof(pathfinding_over_a_graph
                           "lobby door vault, 8[.]00 long"
                           "the door closed: lobby hall bend vault, 20[.]00 long"
                           "both closed: no way"
                           "to itself: lobby, 0[.]00 long"
                           "to a room with no link: no way"
                           "linked: lobby hall bend attic, 17[.]00 long"
                           "start top, 5[.]00 long"
                           "over the grid, straight: 39"
                           "over the grid, walled but for its end: 117"
                           "cheapest with more steps: [[]0, 1, 2, 3[]], cost 3"
                           "past a guess's favorite: [[]0, 2, 3[]], cost 4"
                           "a good guess opens fewer than none True")
  fbengine_add_proof(particles_on_sprites
                           "after 1 second, 20 made, 20 alive"
                           "the oldest's risen True, its alpha True"
                           "the newest's nearly at the start True, almost opaque True"
                           "after 5 seconds, 100 made, about 30 alive True"
                           "in a camera's view, the oldest True"
                           "the same seed goes the same way True"
                           "with the rate at 0, after 2 seconds, 0 alive"
                           "mode UPRIGHT, scale [(]0[.]75, 1[.]25[)], rotation 0[.]5, color [(]0[.]6, 0[.]5, 0[.]4, 0[.]25[)]"
                           "its quad's bounds 1[.]458, 1[.]458, 1[.]458"
                           "in view True, a point behind the camera False"
                           "freed one: 2 left, it's freed True"
                           "100 spread across True"
                           "another seed goes another way True"
                           "a rocket gone, 4 sparks, each UPRIGHT, UPRIGHT"
                           "falling True"
                           "a sprite's pixel, added, out of the fog [(]255, 6[2-6], 0, 255[)]"
                           "beside it [(]255, 0, 0, 255[)]")

  fbengine_add_proof(fixed_function_look
                           "a room's lightmap and diffuse, left: [(]125, 110, 102, 255[)]"
                           "and right, its lightmap's UVs its own: [(]75, 66, 61, 255[)]"
                           "a stage's clamped: [(]120, 120, 120, 255[)]"
                           "four layers, over vertex colors and light: [(]36, 20, 20, 255[)]"
                           "a layer by its alpha: [(]127, 0, 128, 255[)]"
                           "full bright: [(]255, 255, 255, 255[)]"
                           "lit, with no light: [(]0, 0, 0, 255[)]"
                           "vertex colors left out: [(]255, 255, 255, 255[)]"
                           "vertex colors: [(]255, 0, 0, 255[)]"
                           "its back: [(]0, 0, 0, 255[)]"
                           "its back, two sided: [(]255, 255, 255, 255[)]"
                           "half way into fog: [(]128, 128, 128, 255[)]"
                           "out of the fog: [(]255, 255, 255, 255[)]"
                           "per vertex, range over distance, its middle: [(]113, 113, 113, 255[)]"
                           "and its corner: [(]113, 113, 113, 255[)]"
                           "within its range: [(]240, 240, 240, 255[)]"
                           "the ambient light: [(]128, 128, 128, 255[)]"
                           "nine lights, eight lighting: [(]192, 192, 192, 255[)]"
                           "forced alpha: [(]188, 188, 188, 255[)]"
                           "AUTO without a texture's alpha OPAQUE"
                           "so opaque: [(]255, 255, 255, 255[)]"
                           "and with it ALPHA"
                           "blended by the texture's alpha: [(]188, 188, 188, 255[)]"
                           "behind by depth: [(]255, 0, 0, 255[)]"
                           "later by its order: [(]0, 255, 0, 255[)]"
                           "sorted far to near: [(]137, 188, 0, 255[)]"
                           "in stored values, sorted far to near: [(]64, 128, 0, 255[)]"
                           "forced alpha: [(]128, 128, 128, 255[)]"
                           "blended by the texture's alpha: [(]128, 128, 128, 255[)]"
                           "a room's stages, as they were: [(]120, 120, 120, 255[)]"
                           "and in linear light again, forced alpha: [(]188, 188, 188, 255[)]"
                           "a sprite BILLBOARD, 32 across"
                           "a sprite FIXED, [56] across"
                           "a sprite UPRIGHT, 32 across"
                           "a sprite UPRIGHT_YAW, [56] across"
                           "BILLBOARD from above and aside: 32 across, 32 high"
                           "FIXED from above and aside: 0 across, 0 high"
                           "UPRIGHT from above and aside: 32 across, 16 high"
                           "UPRIGHT_YAW from above and aside: 16 across, 8 high")
  fbengine_add_proof(cameras_and_viewports
                           "the left camera's middle: [(]255, 0, 0, 255[)]"
                           "its clear: [(]0, 0, 255, 255[)]"
                           "the right camera's middle: [(]255, 0, 0, 255[)]"
                           "its clear: [(]0, 255, 0, 255[)]"
                           "at 60 degrees, 4 from its middle: [(]255, 0, 0, 255[)]"
                           "at 90 degrees, the same place: [(]0, 0, 255, 255[)]"
                           "at 15 degrees, its corner: [(]255, 0, 0, 255[)]"
                           "past its range: [(]0, 0, 255, 255[)]"
                           "the other's range is its own: [(]255, 0, 0, 255[)]"
                           "in the other's fog: [(]128, 128, 128, 255[)]"
                           "in its own, none: [(]0, 0, 0, 255[)]"
                           "the overlay: [(]255, 255, 0, 255[)]"
                           "the world under it: [(]255, 0, 0, 255[)]"
                           "its clear under it: [(]0, 0, 255, 255[)]"
                           "deeper, not clearing either: [(]255, 0, 0, 255[)]"
                           "deeper, clearing depth: [(]0, 255, 255, 255[)]"
                           "deeper, clearing color: [(]0, 255, 0, 255[)]"
                           "deeper, clearing both: [(]0, 255, 255, 255[)]"
                           "a hidden camera's part stays: [(]0, 0, 255, 255[)]"
                           "the other's changes: [(]255, 0, 255, 255[)]")
  fbengine_add_proof(camera_into_a_texture
                           "the monitor's middle on the screen: [(]255, 0, 0, 255[)]"
                           "its corner: [(]0, 0, 255, 255[)]"
                           "the frame, copied: [(]255, 0, 0, 255[)]"
                           "the screen goes on: [(]0, 0, 0, 255[)]"
                           "and the copy stays: [(]255, 0, 0, 255[)]"
                           "drawn back at half: [(]188, 0, 0, 255[)]"
                           "cleared: [(]10, 20, 30, 255[)]"
                           "drawn into: [(]200, 100, 50, 255[)]"
                           "a part copied: [(]200, 100, 50, 255[)]"
                           "beside it: [(]0, 0, 0, 0[)]"
                           "over itself: [(]200, 100, 50, 255[)]"
                           "drawn back at half, on what's stored: [(]128, 0, 0, 255[)]"
                           "the monitor in stored values, on the screen: [(]128, 0, 0, 255[)]"
                           "and half of it, in linear light: [(]92, 0, 0, 255[)]"
                           "and in stored values: [(]64, 0, 0, 255[)]")
  fbengine_add_proof(bump
                           "a flat map's left: [(]255, 0, 0, 255[)]"
                           "its right: [(]0, 255, 0, 255[)]"
                           "shifted a quarter across: [(]0, 255, 0, 255[)]"
                           "by its green: [(]0, 255, 0, 255[)]"
                           "by M01, down: [(]0, 0, 255, 255[)]"
                           "half its light, by its blue: [(]0, 128, 0, 255[)]"
                           "all of it, offset by half: [(]0, 255, 0, 255[)]"
                           "its matrix moves a texel or so back True")
  fbengine_add_proof(render_settings
                           "by itself, its pixels are within 8 of each other True"
                           "the bias -3"
                           "sharper, they're apart by over 64 True"
                           "blurrier, within 8 True"
                           "1 sample, between 0"
                           "4 samples 4, between over 10 True"
                           "the screen's 4"
                           "on the screen, between over 10 True"
                           "filled 576"
                           "wireframe's lines, some True, under half True"
                           "inside a triangle: [(]0, 0, 0, 255[)]"
                           "validation errors 0")
  fbengine_add_proof(gamma
                           "the sum for adding half 121"
                           "brighter: [(]121, 121, 121, 255[)]"
                           "black stays: [(]0, 0, 0, 255[)]"
                           "white stays: [(]255, 255, 255, 255[)]"
                           "the sum for multiplying by 128 46"
                           "darker: [(]46, 46, 46, 255[)]"
                           "black stays, darkened: [(]0, 0, 0, 255[)]"
                           "white comes to the gray: [(]128, 128, 128, 255[)]"
                           "by a curve: [(]160, 160, 160, 255[)]"
                           "its ends stay: [(]0, 0, 0, 255[)]"
                           "and the other: [(]255, 255, 255, 255[)]"
                           "on what's stored, brighter: [(]150, 150, 150, 255[)]"
                           "white stays: [(]255, 255, 255, 255[)]"
                           "on what's stored, darker: [(]50, 50, 50, 255[)]"
                           "white comes to the gray: [(]128, 128, 128, 255[)]")
  fbengine_add_proof(seeded_random
                           "at first its state 4660"
                           "its first numbers 0[.]35565948, 0[.]7478714, 0[.]31705475, 0[.]40476227, 0[.]5803604"
                           "then its state 1446483090"
                           "the 10000th from 1 399268537"
                           "from 1 to 6 6, 3, 1, 3, 6, 5, 4, 3"
                           "from -3 to 1 1, -1, -3, -2, 1, 0, -1, -1"
                           "from 1 to -3, the same 1, -1, -3, -2, 1, 0, -1, -1"
                           "floats from -5 to 5 4[.]3540, -0[.]8167, -3[.]6881, -1[.]3026"
                           "the same seed the same True, another another True"
                           "a seed of 0 is 1"
                           "a seed with its top bit is 1"
                           "a seed of its top bit alone is 1"
                           "a seed of all ones is 2147483647"
                           "then 7[.]6293945e-6, its state 0, and 7[.]6293945e-6"
                           "the 26th from 42, from 1 to 1000000, 710702"
                           "from 0 to the most 2008760320"
                           "the seed of abc 297"
                           "of 25 a's 553648062"
                           "of an accented e 401"
                           "std's Random from 42 isn't it True")
  fbengine_add_proof(ini_settings
                           "its own key 3"
                           "its sections [[]version, window, keys, room ambience[]]"
                           "a key with a space, in a section with one GFX.map.a[.]rmesh"
                           "width 1280, height 720"
                           "fullscreen True, vsync False"
                           "title \"  a game  \""
                           "scale 1[.]5"
                           "a key given again 57"
                           "missing 24, default, True"
                           "not a number 7"
                           "as written, 194 bytes, its first line version = 3, then [[]window[]]"
                           "read again the same True"
                           "width 1920, volume 0[.]75, title \"  a game  \""
                           "written twice the same True"
                           "not an INI file: line 2")
  fbengine_add_proof(procedural_map
                           "[.][.][.][.][.][.][.][.][.][.][.][.][.][.][.][.][.][.]"
                           "[.][.][.][.][.][.][.][.][.][.][.][.][.][.][.][.]E[.]"
                           "[.][.][.][.][.][.][.][.][.][.][.][.][.][.][.][.]d[.]"
                           "[.][.][.][.][.][.][.]a[.][.]a[.][.][.][.][.]d[.]"
                           "[.][.][.][.][.]hdideieddedg[.]"
                           "[.][.][.][.][.]d[.][.][.][.][.][.][.][.][.][.][.][.]"
                           "[.][.][.][.][.]gdddeddeedfh[.]"
                           "[.][.]c[.][.][.][.][.][.][.][.][.][.][.][.][.]d[.]"
                           "[.]bleeddeddfefdedg[.]"
                           "[.][.]e[.][.][.][.][.][.][.][.][.][.][.][.][.][.][.]"
                           "[.][.]gdeeddeededddg[.][.]"
                           "[.][.][.][.][.][.][.][.][.][.][.][.][.][.][.]d[.][.]"
                           "[.][.][.][.][.][.][.][.][.][.][.][.]a[.][.]e[.][.]"
                           "[.]gdffdeedddeiddh[.][.]"
                           "[.]e[.][.][.][.][.][.][.][.][.][.][.][.][.][.][.][.]"
                           "[.]d[.][.][.][.][.][.][.][.][.][.][.][.][.][.][.][.]"
                           "[.]heedddedh[.][.][.][.][.][.][.][.]"
                           "[.][.][.][.][.][.][.][.][.]S[.][.][.][.][.][.][.][.]"
                           "the seed's 162, its digest 3100503262"
                           "then its state 890016407, and next 81"
                           "the same seed the same True, others others True"
                           "rooms: dead ends 7, 2-ways 82, 3-ways 3, 4-ways 1"
                           "doors 92, open 26"
                           "its 93 cells, 93 of them linked to the start, the exit 87 steps away"
                           "placed 93, overlapping 0, 184 ways across its 92 doors, each from both sides, 0 going nowhere"
                           "the exit's at [(]128, 0, 8[)], E 16,1"
                           "picked from above, the exit True"
                           "an empty cell, nothing True"
                           "of 40 seeds: linked with 5 dead ends 40, with a 4-way 40, apart 40, walkable 40, different 29"
                           "kinds a 84, b 52, c 30, d 1767, e 869, f 299, g 345, h 146, i 164, j 78, k 30, l 10")
  fbengine_add_proof(room_files
                           "its children [[]PIVOT , MESH drawn, MESH collision, PIVOT , PIVOT , PIVOT copies, LIGHT , LIGHT []]"
                           "2 surfaces, the floor's 4 vertices, 2 triangles"
                           "a corner [(]1024, 0, -1024[)], its UVs [(]1, 1[)] and [(]0[.]25, 0[.]5[)], stored [(]128, 64, 32, 255[)]"
                           "its first triangle, 0 2 1 in the file, [(]0, 1, 2[)]"
                           "in the world, across 4[.]000"
                           "the floor's layers MULTIPLY_2 0, ADD 1, MULTIPLY 0"
                           "its diffuse from its own folder True, the ambient everyone's True"
                           "the pane's MULTIPLY 0, ALPHA, two sided True"
                           "the file's [+]z side [(]2[345], 1[567], 1[123], 255[)]"
                           "its -z side [(]4[678], 3[012], 2[234], 255[)]"
                           "the pane over its right [(]1[678], 18[789], [678], 255[)]"
                           "with ambient 15 [(][56][0-9], 3[678], 2[567], 255[)]"
                           "textures loaded, 3, and the second's are the first's True"
                           "from below, the floor's back [(][01], [01], 25[45], 255[)], the pane [(][01], 18[789], 18[678], 255[)]"
                           "both flag 1 [(][01], [01], [01], 255[)]"
                           "flag 2, a name with _LM, adds [(]4[678], 3[012], 2[234], 255[)]"
                           "flag 3 alone, blank white [(]12[789], 6[345], 3[123], 255[)]"
                           "flag 3, its diffuse [(][0-9]+, [45][0-9], 2[456], 255[)]"
                           "none, its vertices [(]12[789], 6[345], 3[123], 255[)]"
                           "a diffuse alone [(][0-9]+, [45][0-9], 2[456], 255[)]"
                           "a lightmap alone [(]18[789], 10[345], 6[123], 255[)]"
                           "a file that isn't there is a slot with nothing [(]18[789], 10[345], 6[123], 255[)]"
                           "a texture's blend stays its first [(]20[012], [0-9]+, [45][0-9], 255[)]"
                           "another folder's of the same name, the first's [(]20[012], 1?[0-9]+, 5[012], 255[)]"
                           "placed, showing True, the template False"
                           "what falls rests on its floor 40[.]000, 0[.]251, 0[.]000"
                           "through the pane, to the floor 42[.]000, 0[.]251, 0[.]000"
                           "against the hidden wall 39[.]251, 1[.]000, 0[.]000"
                           "inside the hidden wall 38[.]251, 1[.]000, 0[.]000"
                           "from under the floor 43[.]000, -0[.]251, 0[.]000"
                           "picked from above, past the pane, collision copy at y 0[.]000"
                           "turned, its wall's met at z 2[.]000 by collision"
                           "its waypoint 78[.]000, 0[.]000, -2[.]000, its screen 80[.]000, 1[.]000, -1[.]000, its 8th emitter 80[.]000, 0[.]000, -8[.]000"
                           "its trigger by the door, 76[.]000, 0[.]000, -4[.]000 to 78[.]000, 2[.]000, -2[.]000"
                           "its first light 79[.]000, 2[.]000, -1[.]000, LIGHT"
                           "triggers 1: by the door, 2[.]000, 0[.]000, -4[.]000 to 4[.]000, 2[.]000, -2[.]000"
                           "waypoints 2[.]000, 0[.]000, -2[.]000"
                           "screens 1: note[.]png at 1[.]000, 1[.]000, -?0[.]000"
                           "lights 2, each POINT POINT"
                           "range 2[.]000, at 1[.]000, 2[.]000, -1[.]000, intensity 0[.]400, and 1[.]000"
                           "its color [(]255, 128, 0, 255[)]"
                           "emitters 8 of 12: the 8th 8, range 6[.]5, at 8[.]000, 0[.]000, -?0[.]000"
                           "props 1: chair[.]mesh, at [(]16, 32, -64[)], turned [(]0, 90, 0[)], size [(]2, 2, 2[)]"
                           "with no triggers, 0, and the rest the same 1 2"
                           "a wrong header: a room file: it starts RoomMesh, not Mesh"
                           "cut short anywhere, it throws True of 1721 bytes"
                           "an entity it doesn't know: a room file: its entities have no dragon"
                           "a string of -1: a room file: a string can't be -1 long"
                           "a triangle past its vertices: a room file: a triangle's 9, 0, 0 aren't all of its 0 vertices")
  # A proof that says all it should and then ends badly fails.
  add_test(NAME proof.an_exit_code_fails
           COMMAND "${CMAKE_COMMAND}" "-DFB=${FBENGINE_FB}" "-DFOLDER=${CMAKE_CURRENT_SOURCE_DIR}/proof_exit_fails"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/proof_exit.cmake")
  set_tests_properties(proof.an_exit_code_fails PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nall it should\r?\n$"
                       FAIL_REGULAR_EXPRESSION "exit code [^0]" WILL_FAIL True TIMEOUT 120)
  # With no GPU, a room file's proof is skipped, not failed.
  add_test(NAME proof.room_files_no_gpu
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/proof/room_files")
  set_tests_properties(proof.room_files_no_gpu PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;FBENGINE_NO_GPU=1"
                       PASS_REGULAR_EXPRESSION "\nno GPU here\r?\n$" TIMEOUT 120)
  # What each --watch's last frame shows, read off it by the sample and checked here, what the walk
  # pressed included; a color a GPU can round another way is within 1.
  set_tests_properties(proof.swept_collisions_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nthe ball's inside True, drawn [(]124, 218, 242, 255[)]\r?\nits walls, 1 touched, each its color True\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.picks_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nunder its point: floor at -5[.]96, 0[.]00, 0[.]71, facing 0[.]00, 1[.]00, 0[.]00, 0[.]01 along\r?\nmarked [(]255, 89, 89, 255[)]\r?\nthe ball, the crate, the floor: [(]124, 218, 242, 255[)], [(]231, 188, 124, 255[)], [(]108, 108, 124, 255[)]\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.in_view_by_bounds_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\n3 green and 9 red, as the sums have them True\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.scene_trees_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\na door [(]218, 170, 124, 255[)], its handle on the side it's seen from False, drawn so True\r?\nthe copy's door hidden 20 frames of 40, drawn just while it's not True\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.meshes_in_code_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nits red corner True, its blue True\r?\nthe wall through its corner of no alpha True\r?\nlit by what it's worked out from its rise True\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.binary_saves_and_maps_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\n4 rooms, each drawn by what's read back True, the first's light 0[.]95\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.pathfinding_over_a_graph_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nits way 49 squares from 0 to 575, by a wall of 17, each its color True\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.particles_on_sprites_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\n28 alive of its smoke\r?\nover where it starts brighter True, the corner [(]39, 39, 48, 255[)]\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.fixed_function_look_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\na room's lightmap and diffuse, fogged, left: [(]103, 92, 89, 255[)]\r?\nthen: [(]66, 59, 58, 255[)]\r?\nthe pane's half over what's behind: [(]89, 141, 168, 255[)]\r?\nthe crate's lit True\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.cameras_and_viewports_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nits clears: [(]39, 39, 89, 255[)], [(]39, 89, 39, 255[)]\r?\nthe cube on the left [(]231, 124, 89, 255[)], on the right fogged True\r?\nthe badge over them [(]255, 231, 0, 255[)]\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.camera_into_a_texture_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nthe cube on the monitor's greenest True\r?\nbeyond the monitor [(]39, 39, 48, 255[)]\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.bump_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nits bricks moved True, each a brick's True, the middle [(][0-9]+, [0-9]+, [0-9]+, 255[)]\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.render_settings_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nby its keys: bias 0, samples 1, wireframe True\r?\nthe floor's far checks sharper at -2 than 2 True, by [0-9]+ and [0-9]+\r?\nthe cube's pixels fewer in wireframe True\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.gamma_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nby its keys: gamma 1[.]25\r?\nits bars of 0, 136, and 255: [(]0, 0, 0, 255[)], [(]15[0-2], 15[0-2], 15[0-2], 255[)], [(]255, 255, 255, 255[)]\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.seeded_random_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nits bars 217, 284, 8000\r?\nthe foot of its first [(]76, 141, 255, 255[)], each bar as tall as its count True, the tallest 360\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.ini_settings_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nthe volume 3\r?\nits bar to [(]76, 141, 255, 255[)], past it [(]24, 28, 36, 255[)]\r?\n5 lines of text\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.procedural_map_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nits last seed 5, 103 rooms of 103, in the world 103\r?\n91 of its rooms drawn where they are, its corner [(]39, 39, 48, 255[)]\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.room_files_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nthe middle room's forward -1[.]000, -?0[.]000, 0[.]029\r?\n3 of its 3 copies drawn, its corner [(]39, 39, 48, 255[)]\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.sprites_over_a_scene_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nthe rect over what's behind [(]16[2-4], 3[5-7], (3[89]|40), 255[)], the oval's middle [(]16[2-4], 3[5-7], (3[89]|40), 255[)], off its corner [(]24, 28, 36, 255[)]\r?\nthe strip's first frame [(]255, 200, 80, 255[)], off its block [(]24, 28, 36, 255[)]\r?\nturned [(]255, 200, 80, 255[)], off it [(]120, 130, 150, 255[)]\r?\nthe mesh under them [(]120, 130, 150, 255[)], the text's ink True\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.pixels_of_images_and_textures_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\n10240 written: at 10, 3 [(]218, 20, 6, 255[)], by its sum [(]218, 20, 6, 255[)]\r?\nthe last row it reached and the next: [(]13, 0, 158, 255[)], [(]0, 0, 0, 255[)]\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.display_modes_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nWINDOWED, its squares [(]76, 141, 255, 255[)], [(]76, 141, 255, 255[)], off them [(]24, 28, 36, 255[)], [(]24, 28, 36, 255[)]\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.text_input_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\ntyped \"Cabin\", shown True\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.debug_ui_and_console_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nat speed 15, its square [(]76, 141, 255, 255[)], past it [(]24, 28, 36, 255[)]\r?\nnoclip True, console True\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.textures_by_their_settings_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\neach one: [(]200, 0, 0, 255[)], [(]0, 200, 0, 255[)], [(]188, 188, 255, 255[)], [(]255, 255, 255, 255[)], [(]0, 0, 200, 255[)], [(]255, 255, 255, 255[)], [(]255, 128, 0, 255[)], [(]255, 0, 255, 255[)], [(]255, 255, 255, 255[)]\r?\nthe strip's frame 2: [(]0, 0, 255, 255[)]\r?\nwatched 40 frames\r?\n$")

  # Their --watch, which a person watches in a window (tools/show.py), walked here offscreen, a step
  # each frame.
  add_test(NAME examples.controls_watch
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/controls"
                   -- --watch --frames 30)
  set_tests_properties(examples.controls_watch PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "\nwatched 30 frames, moved True, jumped True\r?\n$")
  add_test(NAME examples.room_watch
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/room"
                   -- --watch --frames 30)
  set_tests_properties(examples.room_watch PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "\nwatched 30 frames, [0-9]+ draws in the last\r?\n$")
  add_test(NAME examples.menu_watch
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/menu"
                   -- --watch --frames 60)
  set_tests_properties(examples.menu_watch PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "\nwatched 60 frames: options True, playing True, console True, menu again True\r?\n$")
  # Escape and the close button end a --watch; on the menu's options Escape goes back, on the game to
  # the menu, and on the menu, there already, it ends it.
  set(fbengine_controls "${PROJECT_SOURCE_DIR}/examples/controls")
  set(fbengine_menu "${PROJECT_SOURCE_DIR}/examples/menu")
  fbengine_add_watched(examples.controls_escape "${fbengine_controls}" "--press-at;3" "\nwatched 4 frames, ")
  fbengine_add_watched(examples.controls_close "${fbengine_controls}" "--close-at;3" "\nwatched 4 frames, ")
  fbengine_add_watched(examples.menu_close "${fbengine_menu}" "--close-at;3" "\nwatched 3 frames: ")
  fbengine_add_watched(examples.menu_escape_on_options "${fbengine_menu}" "--press-at;5" "\nwatched 40 frames: ")
  fbengine_add_watched(examples.menu_escape_on_options_then_menu "${fbengine_menu}"
                       "--press-at;5;--press-at;7" "\nwatched 7 frames: options True, playing False")
  fbengine_add_watched(examples.menu_escape_on_game_then_menu "${fbengine_menu}"
                       "--press-at;17;--press-at;19" "\nwatched 19 frames: options True, playing True")

  add_test(NAME examples.menu
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/menu")
  set_tests_properties(examples.menu PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "Running menu[^\n]*\noptions open: True\r?\nvolume 0[.]65, fullscreen True, name Ada\r?\nmenu again: True\r?\nplaying: True, focus none\r?\na jump: score 100\r?\nconsole: [[]> give 50, gave 50[]], score 150, typed \" \"\r?\nclosed, a jump: score 250\r?\n$")
endif()

# The packages' tests draw on the tests' Vulkan too.
if(FBENGINE_FB_TESTS)
  foreach(package IN LISTS fbengine_packages)
    if(EXISTS "${package}/project.toml")
      cmake_path(GET package FILENAME name)
      set_property(TEST ${name}.package APPEND PROPERTY ENVIRONMENT ${fbengine_vulkan_environment})
    endif()
  endforeach()
endif()

# Texture's panics, each a case of tests/texture/panics run on its own, which has to panic saying
# what it does, on the tests' Vulkan.
if(FBENGINE_FB_TESTS)
  set(fbengine_texture_panics
      "wrapped-size=32768 by 16385 pixels of RGBA16F are 4295229440 bytes, not 262144"
      "huge-read=level 0's 2147483648 bytes are more than an array holds"
      "sprite-frame=the sprite's frames are 0 to 1, not 2"
      "canvas-freed=DrawRect on a Texture that was freed"
      "pixels-freed=Get on Pixels whose Texture was freed"
      "pixels-stopped=Set on Pixels whose Texture was freed"
      "pixels-drawn-over=the GPU changed the texture after its pixels came to the CPU, so they're put there again to be written"
      "lod-bias=a LOD bias is -16 to 16, not 17"
      "anisotropy=anisotropy is 1, for none, to 16, not 0.5"
      "screen-samples=the screen draws 1, 2, 4, or 8 samples a pixel, not 3")
  foreach(case IN LISTS fbengine_texture_panics)
    string(FIND "${case}" "=" split)
    string(SUBSTRING "${case}" 0 ${split} name)
    math(EXPR split "${split} + 1")
    string(SUBSTRING "${case}" ${split} -1 message)
    add_test(NAME texture.panics.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/texture/panics"
                     -- ${name})
    set_tests_properties(texture.panics.${name} PROPERTIES
                         PASS_REGULAR_EXPRESSION "panic: ${message}"
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}")
  endforeach()
endif()

# The scene, through fbengine's C API, with no window or GPU.
add_executable(fbengine_scene_tests scene/world_tests.cpp scene/transform_tests.cpp
               scene/camera_tests.cpp scene/light_tests.cpp scene/geometry_tests.cpp
               scene/material_tests.cpp scene/bvh_tests.cpp scene/collision_tests.cpp
               scene/handles_tests.cpp scene/pick_tests.cpp scene/sprite_tests.cpp
               scene/plane_tests.cpp scene/draw_tests.cpp
               scene/terrain_tests.cpp scene/clusters_tests.cpp scene/effects_tests.cpp
               "${PROJECT_SOURCE_DIR}/native/fbengine/scene/bvh.cpp")
target_link_libraries(fbengine_scene_tests PRIVATE fbengine_native Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_scene_tests)
# bvh.cpp, built here too, rounds as the DLL's does.
if(NOT MSVC)
  target_compile_options(fbengine_scene_tests PRIVATE -ffp-contract=off)
endif()
# A program that ends at once after its moves, its workers still running: it has to end.
add_executable(fbengine_scene_ends scene/ends_after_moves.cpp)
target_link_libraries(fbengine_scene_ends PRIVATE fbengine_native)
fbengine_set_warnings(fbengine_scene_ends)
add_test(NAME scene.ends_after_moves COMMAND fbengine_scene_ends)
set_tests_properties(scene.ends_after_moves PROPERTIES
                     PASS_REGULAR_EXPRESSION "moved" TIMEOUT 60)
set_property(TEST scene.ends_after_moves PROPERTY ENVIRONMENT_MODIFICATION
             "PATH=path_list_prepend:$<TARGET_FILE_DIR:fbengine_native>")
if(fbengine_test_dll_paths)
  set_property(TEST scene.ends_after_moves APPEND PROPERTY ENVIRONMENT_MODIFICATION
               "PATH=path_list_prepend:${fbengine_test_dll_paths}")
endif()
# Another thread's exit() during moves, on systems where that ends statics under them: it has to
# end with its code, however far they've got.
if(UNIX)
  add_executable(fbengine_scene_ends_during scene/ends_during_moves.cpp)
  target_link_libraries(fbengine_scene_ends_during PRIVATE fbengine_native)
  fbengine_set_warnings(fbengine_scene_ends_during)
  add_test(NAME scene.ends_during_moves COMMAND fbengine_scene_ends_during)
  set_tests_properties(scene.ends_during_moves PROPERTIES
                       PASS_REGULAR_EXPRESSION "ended" TIMEOUT 300)
endif()
catch_discover_tests(fbengine_scene_tests TEST_PREFIX "scene."
                     DL_PATHS "${fbengine_test_dll_paths}" "${PROJECT_BINARY_DIR}/native/fbengine")

# The scene's panics, each a case of tests/scene/panics run on its own, which has to panic saying
# what it does.
if(FBENGINE_FB_TESTS)
  set(fbengine_scene_panics
      "particles-colors=SetColors on a Particles: a particle's alpha is 0 to 1, not 2"
      "particles-roll=SetRoll on a Particles: a particle's roll is up to 0 to Pi each way, not -1"
      "decal-limit=Decal: a decal's limit is above 0 and up to Pi / 2, not 2"
      "sky-texture-brightness=SetSkyTexture on a World: a sky's brightness is 0 or more, not -1"
      "instance-out-of-range=Get on an Instances: an Instances has 0 of them, not one at 0"
      "instance-set-out-of-range=SetOne on an Instances: an Instances has 0 of them, not one at 0"
      "lod-past-next=SetLod on a Geometry: a geometry has 0 levels of detail, and 1 is past the next"
      "lod-share=SetLod on a Geometry: a level of detail's share is above 0 and below the one before it, not 0"
      "lod-of-itself=SetLod on a Geometry: a level of detail's geometry has none of its own"
      "particles-capacity=SetCapacity on a Particles: an emitter holds 1 to 1048576 particles, not 0"
      "particles-rate=SetRate on a Particles: particles are born at 0 a second or more, not -1"
      "particles-life=SetLife on a Particles: a particle lives above 0 seconds, the least to the most, up to 512, not 2 to 1"
      "particles-motion=SetMotion on a Particles: a particle's speeds are 0 or more, least to most, its spread 0 to Pi from its axis, and its spin 0 or more, not 2 to 1, 0, 0"
      "particles-forces=SetForces on a Particles: a particle's gravity is finite and its drag 0 or more, not -1"
      "particles-sizes=SetSizes on a Particles: a particle's half-size is 0 or more, not -1 to 0"
      "particles-burst=Burst on a Particles: a burst is 0 particles or more, not -1"
      "sky-brightness=SetAtmosphere on a World: a sky's brightness is 0 or more, not -1"
      "sky-sun=SetAtmosphere on a World: a sky's sun is a directional light"
      "decal-box=Decal: a decal's box has an inverse"
      "shadow-atlas-limit=SetShadowAtlasLimit: a shadow atlas's side is at most 256 to 8192, not 128"
      "shadow-size=SetShadowSize on a Light: a shadow's tile is 16 to 8192 texels a side, not 8"
      "cascades=SetCascades on a Light: a light has 1 to 4 cascades, not 5"
      "shadow-distance=SetShadowDistance on a Light: a shadow reaches a distance above 0, not 0"
      "shadow-bias=SetShadowBias on a Light: a shadow's biases are 0 or more, not -1 and 0"
      "render-no-target=Render on a World: a camera with no target draws into the screen, and there's no window open"
      "render-bad-tween=Render on a World: a tween is 0 to 1, not 2"
      "lit-by-freed=LitBy on a World that was freed"
      "map-as-color=SetMap on a Material: a material's metal-rough, normal, and occlusion maps are RGBA8_LINEAR"
      "tangent-w=SetTangents on a Geometry: a tangent's w is 1 or -1, not 0.25"
      "layer-uv-zero=SetLayerUv on a Material: a layer's UV scale is finite and not 0, not \\(0, 1\\)"
      "layer-texture-out-of-range=SetTexture on a Material: a material's layers are 0 to 7"
      "joints-wrong-count=SetJoints on a Geometry: a surface of 24 vertices has as many joints, or none, not 1"
      "freed-entity=SetName on a Pivot that was freed"
      "freed-with-parent=Name on a Pivot that was freed"
      "new-under-freed=New on a Pivot that was freed"
      "freed-world=Roots on a World that was freed"
      "current-freed=SetCurrent on a World that was freed"
      "free-main=Free on the main World: the main World stays"
      "own-parent=SetParent on a Pivot: an entity can't be its own parent"
      "under-itself=SetParent on a Pivot: an entity can't go under what's under it"
      "child-out-of-range=Child\\(2\\) on a Pivot with 2 children"
      "data-freed=Data on a Pivot that was freed"
      "poses-count=SetPoses takes a pose for each entity, not 0 for 1"
      "poses-freed=Poses on a Pivot that was freed"
      "into-nothing=TransformPoint: the entity to transform into is scaled to nothing on an axis"
      "turn-freed=Turn on a Pivot that was freed"
      "point-at-freed=PointAt on a Pivot that was freed"
      "distance-freed=DistanceTo on a Pivot that was freed"
      "yaw-freed=YawTo on a Pivot that was freed"
      "pitch-freed=PitchTo on a Pivot that was freed"
      "field-of-view=SetFieldOfView on a Camera: a field of view is between 0 and Pi, not 0"
      "range=SetRange on a Camera: a camera's range is 0 < near < far, finite, not 5 to 1"
      "viewport=SetViewport on a Camera: a viewport has a finite place and a size above 0"
      "no-window=Project on a Camera: the camera's viewport has no size: open the window, or give it a viewport"
      "horizontal-no-window=HorizontalFieldOfView on a Camera: the camera's viewport has no size"
      "camera-freed=SetOrder on a Camera that was freed"
      "ambient-occlusion-negative=SetAmbientOcclusion on a Camera: ambient occlusion's strength is 0 or more, not -1"
      "ambient-occlusion-radius-zero=SetAmbientOcclusionRadius on a Camera: ambient occlusion's radius is 0.0001 or more, not 0"
      "intensity=SetIntensity on a Light: a light's intensity is 0 or more, not -1"
      "light-range=SetRange on a Light: a light's range is above 0, not 0"
      "cone=SetCone on a Light: a spot light's cone is 0 <= inner <= outer <= Pi / 2"
      "fog-range=SetFogRange on a Camera: a fog's range is 0 <= start < end, finite, not 5 to 5"
      "fog-density=SetFogDensity on a Camera: a fog's density is 0 or more"
      "bloom-negative=SetBloom on a Camera: a camera's bloom's intensity and threshold are 0 or more, not -1 and 1"
      "bloom-not-finite=SetBloom on a Camera: a camera's bloom's intensity and threshold are 0 or more, not 1 and"
      "bloom-freed=SetBloom on a Camera that was freed"
      "grade-power=SetGrade on a Camera: a camera's color grade's slope is 0 or more, and its power above 0"
      "grade-freed=Grade on a Camera that was freed"
      "exposure-negative=SetExposure on a Camera: a camera's exposure is 0 or more, not -1"
      "gamma-zero=SetGamma on a Camera: a camera's gamma is above 0, not 0"
      "stored-on-hdr=SetStoredValues on a Camera: an HDR camera doesn't draw in stored values"
      "hdr-on-stored=SetHdr on a Camera: a camera that draws in stored values isn't HDR"
      "tonemap-range=the value isn't one of Tonemap's"
      "exposure-freed=SetExposure on a Camera that was freed"
      "light-freed=SetColor on a Light that was freed"
      "segments=Sphere: a shape has 3 to 100 segments, not 2"
      "triangle-index=AddTriangle on a Geometry: the surface's vertices are 0 to 2, not 5"
      "triangle-count=AddTriangles on a Geometry: triangles take indices three at a time, not 4"
      "surface-index=Surface\\(1\\) on a Geometry with 1 surface"
      "vertex-index=Vertex on a Geometry: the surface's vertices are 0 to 2, not 3 at "
      "uv-set=VertexUv on a Geometry: a vertex's UV sets are 0 and 1, not 2"
      "fit-backward=Fit on a Geometry: a box to fit into has its least corner at or below its greatest, not \\(1, 0, 0\\) to \\(0, 1, 1\\)"
      "geometry-freed=AddSurface on a Geometry that was freed"
      "mesh-freed-geometry=SetGeometry on a Geometry that was freed"
      "shininess=SetShininess on a Material: a material's shininess is 0 to 1, not 2"
      "alpha-cutoff=SetAlphaCutoff on a Material: an alpha cutoff is 0 to 1, not -1"
      "layer=SetLayer on a Material: a material's layers are 0 to 7"
      "layer-uv-set=SetLayer on a Material: a layer's UV set is 0 or 1, not 2"
      "material-alpha=SetAlpha on a Material: a material's alpha is 0 to 1, not 2"
      "color-alpha=SetColor on a Material: a material's alpha is 0 to 1, not 1.5"
      "mesh-alpha=SetAlpha on a Mesh: a mesh's alpha is 0 to 1, not -1"
      "material-freed=SetOrder on a Material that was freed"
      "surface-freed-material=SetMaterial on a Material that was freed"
      "collision-group=SetCollisionGroup on a Pivot: a collision group is 0 to 1023, not 1024"
      "collision-radius=SetCollisionRadius on a Pivot: collision radii are above 0, not 0 and 1"
      "collision-box=SetCollisionBox on a Pivot: a collision box has its least corner at or below its greatest"
      "step-height=SetStepHeight on a Pivot: a step height is 0 or more, not -1"
      "response-group=SetCollisionResponse on a World: group 0 is no group, which collides with nothing"
      "iterations=SetCollisionIterations on a World: a move touches 1 to 64 times, not 0"
      "skin=SetCollisionSkin on a World: a collision skin is 0 to 0.1, not 1"
      "collide-freed=Collide on a World that was freed"
      "pick-radius=Pick on the main World: a pick's radius is 0 or more, not -1"
      "pick-freed=Pick on a Pivot that was freed"
      "camera-pick-no-window=Pick on a Camera: the camera's viewport has no size"
      "max-slope=SetMaxSlope on a Pivot: a max slope is 0 to Pi / 2, not 2"
      "drawn-blend=DrawnBlend on a Mesh: the geometry's surfaces are 0 to 0, not 99"
      "screen-ray-freed=ScreenRay on a Camera that was freed"
      "screen-ray-no-window=ScreenRay on a Camera: the camera's viewport has no size"
      "view-frustum-freed=ViewFrustum on a Camera that was freed"
      "view-frustum-no-window=ViewFrustum on a Camera: the camera's viewport has no size"
      "camera-pick-freed=Pick on a Camera that was freed"
      "sprite-scale=SetSpriteScale on a Sprite: a sprite's size is 0 or more, finite, not -1 by 1"
      "sprite-freed=SetHandle on a Sprite that was freed"
      "tween-range=TweenedMatrix on a Pivot: a tween is 0 to 1, not 2"
      "view-tween-range=TweenedViewMatrix on a Camera: a tween is 0 to 1, not -1"
      "capture-freed=Capture on a World that was freed"
      "fixed-step-rate=FixedUpdate: a rate is above 0 steps a second, not 0"
      "fixed-step-most=FixedUpdate: a frame takes at most 1 step or more, not 0"
      "fixed-step-steps=Steps on a FixedUpdate: a frame takes 0 seconds or more, finite, not -1"
      "fixed-step-infinite=Steps on a FixedUpdate: a frame takes 0 seconds or more, finite, not Inf"
      "plane-segments=SetSegments on a GroundPlane: a ground plane has 1 to 16 segments, not 17"
      "auto-fade=SetAutoFade on a Pivot: a fade is 0 <= start < end, finite, or 0 and 0 for none, not 5 to 1"
      "mirror-freed=Reflection on a Mirror that was freed"
      "intersects-freed=Intersects on a Mesh that was freed"
      "draws-tween=Draws on a Camera: a tween is 0 to 1, not 2"
      "draws-no-window=Draws on a Camera: the camera's viewport has no size"
      "draw-projection-no-window=DrawProjection on a Camera: the camera's viewport has no size"
      "cull-box=SetCullBox on a Geometry: a cull box has its least corner at or below its greatest"
      "terrain-size=SetSize on a Terrain: a terrain is 1 to 4096 squares a side, not 0"
      "terrain-corner=SetHeight on a Terrain: a terrain's corners are 0 to 4 across and along, not \\(5, 0\\)"
      "terrain-heights=SetHeights on a Terrain: a terrain 2 squares a side has 9 heights, not 2")
  foreach(case IN LISTS fbengine_scene_panics)
    string(FIND "${case}" "=" split)
    string(SUBSTRING "${case}" 0 ${split} name)
    math(EXPR split "${split} + 1")
    string(SUBSTRING "${case}" ${split} -1 message)
    add_test(NAME scene.panics.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/scene/panics"
                     -- ${name})
    set_tests_properties(scene.panics.${name} PROPERTIES PASS_REGULAR_EXPRESSION "panic: ${message}")
  endforeach()
endif()

# fbengine_physics' C++ that's subtle, apart from its DLL.
add_executable(fbengine_physics_tests physics/handles_tests.cpp)
target_include_directories(fbengine_physics_tests PRIVATE
                           "${PROJECT_SOURCE_DIR}/native/fbengine_physics")
target_link_libraries(fbengine_physics_tests PRIVATE Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_physics_tests)
catch_discover_tests(fbengine_physics_tests TEST_PREFIX "physics."
                     DL_PATHS "${fbengine_test_dll_paths}")

# fbengine_physics' steps, through its DLL, which the tests find where CMake built it.
add_executable(fbengine_physics_step_tests physics/step_tests.cpp physics/determinism_tests.cpp
               physics/casts_tests.cpp)
target_link_libraries(fbengine_physics_step_tests PRIVATE fbengine_physics_native
                      Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_physics_step_tests)
catch_discover_tests(fbengine_physics_step_tests TEST_PREFIX "physics."
                     DL_PATHS "${fbengine_test_dll_paths};$<TARGET_FILE_DIR:fbengine_physics_native>")

# fbengine_physics' own C++, built into its test, for what only its insides show.
file(GLOB fbengine_physics_sources CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/native/fbengine_physics/*/*.cpp")
add_executable(fbengine_physics_internal_tests physics/contacts_tests.cpp
               ${fbengine_physics_sources} physics/float_mode_tests.cpp)
target_include_directories(fbengine_physics_internal_tests PRIVATE
                           "${PROJECT_SOURCE_DIR}/native/fbengine_physics")
target_compile_definitions(fbengine_physics_internal_tests PRIVATE FBE_PHYSICS_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}")
target_link_libraries(fbengine_physics_internal_tests PRIVATE Jolt Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_physics_internal_tests)
catch_discover_tests(fbengine_physics_internal_tests TEST_PREFIX "physics."
                     DL_PATHS "${fbengine_test_dll_paths}")

# fbengine_physics' examples, checked against what they print.
if(FBENGINE_FB_TESTS)
  add_test(NAME examples.falling_boxes
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/falling_boxes")
  set_tests_properties(examples.falling_boxes PROPERTIES PASS_REGULAR_EXPRESSION
                       "\n200 boxes fell, 200 came to rest, 100 on others\r?\n$")
  fbengine_add_watched(examples.falling_boxes.watch "${PROJECT_SOURCE_DIR}/examples/falling_boxes" ""
                       "\n201 drawn, the highest box's mesh has fallen: True\r?\nwatched 40 frames\r?\n$")
  fbengine_add_watched(examples.falling_boxes.escape "${PROJECT_SOURCE_DIR}/examples/falling_boxes"
                       "--press-at;1" "\nwatched 2 frames\r?\n$")
  fbengine_add_watched(examples.falling_boxes.close "${PROJECT_SOURCE_DIR}/examples/falling_boxes"
                       "--close-at;1" "\nwatched 2 frames\r?\n$")
  add_test(NAME examples.door_and_trigger
           COMMAND "${FBENGINE_FB}" --color never run
                   "${PROJECT_SOURCE_DIR}/examples/door_and_trigger")
  set_tests_properties(examples.door_and_trigger PROPERTIES PASS_REGULAR_EXPRESSION
                       "\nthe door swung open: True\r?\nthe player walked into the room: True, and is in it: True\r?\n$")
  add_test(NAME examples.video_player
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/video_player")
  set_tests_properties(examples.video_player PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen"
                       PASS_REGULAR_EXPRESSION "\nit played to its end: True, in 52 frames shown\r?\n$")
  add_test(NAME examples.video_player.watch
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/video_player"
                   -- --watch --steps 120)
  set_tests_properties(examples.video_player.watch PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       TIMEOUT 300
                       PASS_REGULAR_EXPRESSION "\nwatched 52 frames, 53 shown, 1 loops, ended: False\r?\n$")
  add_test(NAME examples.video_player.usage
           COMMAND "${CMAKE_COMMAND}" "-DFB=${FBENGINE_FB}" "-DEXAMPLE=${PROJECT_SOURCE_DIR}/examples/video_player"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/video/player_usage.cmake")
  set_tests_properties(examples.video_player.usage PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen")
  fbengine_add_proof(two_videos_at_startup
                          "the first: 64 by 48, 2.08 s, its own sound True"
                          "at 0.25 s of its sound, red"
                          "at 0.75 s of its sound, green"
                          "at 1.25 s of its sound, blue"
                          "at 1.75 s of its sound, white"
                          "it ended True, heard True, its clock on its sound True"
                          "the second: its own sound False, then one beside it"
                          "at 0.25 s of its sound, red"
                          "at 0.75 s of its sound, green"
                          "at 1.25 s of its sound, blue"
                          "at 1.75 s of its sound, white"
                          "it ended True, heard True, its clock on its sound True"
                          "fitted to 200 by 96: 128 by 96, at 36, 0"
                          "its bars black, black, its middle white"
                          "skipped at half a second: heard True, then False")
  set_tests_properties(proof.two_videos_at_startup_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nthe 1th video, at its frame [0-9]+\r?\nwatched 40 frames\r?\n$")
  fbengine_add_watched(proof.two_videos_at_startup_skipped
                       "${PROJECT_SOURCE_DIR}/examples/proof/two_videos_at_startup" "--skip-at;10"
                       "\nthe 2th video, at its frame [0-9]+\r?\nwatched 40 frames\r?\n$")
  fbengine_add_proof(stack_and_joints
                          "the stack stands, at rest: True"
                          "the block's held where it was: True"
                          "the door's opened a quarter turn: True"
                          "the lift's 4 m up: True"
                          "the crate's on it: True"
                          "the fan's turning at its motor's speed: True"
                          "let go, the block's knocked at least five of the stack away: True"
                          "the chain's held together: True"
                          "the door's swung back: True"
                          "the lift's come back down: True"
                          "every entity's on its body: True"
                          "twice over, it all ends alike: True")
  fbengine_add_watched(proof.stack_and_joints_drawn "${PROJECT_SOURCE_DIR}/examples/proof/stack_and_joints"
                       "--again-at;20"
                       "\n32 of 32 pieces drawn\r?\nthe block's drawn red: True\r?\nwatched 40 frames\r?\n$")
  add_test(NAME examples.crate_on_a_lift
           COMMAND "${FBENGINE_FB}" --color never run
                   "${PROJECT_SOURCE_DIR}/examples/crate_on_a_lift")
  set_tests_properties(examples.crate_on_a_lift PROPERTIES PASS_REGULAR_EXPRESSION
                       "\nthe crate's entity is where its body is: True\r?\nthe crate rode up with the lift: True\r?\n$")
endif()

# Shapes made in a process before any world, as shape_test's are when it runs alone.
if(FBENGINE_FB_TESTS)
  add_test(NAME fbengine_physics.shapes_alone
           COMMAND "${FBENGINE_FB}" test "${PROJECT_SOURCE_DIR}/packages/fbengine_physics"
                   --filter shape_test)
endif()

# What fbengine_physics panics for, each by a run of tests/physics/panics, which says it.
if(FBENGINE_FB_TESTS)
  foreach(case IN ITEMS "step-nan;not nan" "step-infinity;not inf" "step-huge;not 1e\\+30"
                        "step-freed;this PhysicsWorld was freed")
    list(GET case 0 name)
    list(GET case 1 said)
    add_test(NAME physics.panics.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/physics/panics"
                     -- ${name})
    # A minute, past which a run's taken to hang as it ends.
    set_tests_properties(physics.panics.${name} PROPERTIES PASS_REGULAR_EXPRESSION
                         "panic: PhysicsWorld\\.Step: [^\n]*${said}" TIMEOUT 60)
  endforeach()
  # Two bodies of a world keeping one entity.
  foreach(case IN ITEMS "drive-taken;Drive" "follow-taken;Follow")
    list(GET case 0 name)
    list(GET case 1 said)
    add_test(NAME physics.panics.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/physics/panics"
                     -- ${name})
    set_tests_properties(physics.panics.${name} PROPERTIES PASS_REGULAR_EXPRESSION
                         "panic: Body\\.${said}: another body of this world already keeps" TIMEOUT 60)
  endforeach()
  # A world still alive as a panic ends the program, which then exits as a panic does.
  add_test(NAME physics.panics.with-a-world
           COMMAND "${CMAKE_COMMAND}" "-DFB=${FBENGINE_FB}"
                   "-DPROGRAM=${CMAKE_CURRENT_SOURCE_DIR}/physics/panics" -DCASE=with-a-world
                   -DCODE=101 -P "${CMAKE_CURRENT_SOURCE_DIR}/physics/exits.cmake")
  set_tests_properties(physics.panics.with-a-world PROPERTIES TIMEOUT 60)
endif()

# A program that makes a world and ends at once, as a game that panics as it starts does, run a few
# at a time, ends.cmake says how many, again and again on Jolt's most threads: each run ends, rather than its DLL's end waiting for
# ever on a lock of a thread stopped as it started.
add_executable(fbengine_physics_ends physics/ends.cpp)
target_link_libraries(fbengine_physics_ends PRIVATE fbengine_physics_native)
fbengine_set_warnings(fbengine_physics_ends)
add_test(NAME physics.ends
         COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:fbengine_physics_ends>"
                 -DRUNS=150 "-DDLLS=$<TARGET_FILE_DIR:fbengine_physics_native>;${fbengine_test_dll_paths}"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/physics/ends.cmake")
set_tests_properties(physics.ends PROPERTIES TIMEOUT 600)
# The job system, built into the tests from its own source, since the DLL exports only its C API.
add_executable(fbengine_jobs_tests jobs/jobs_tests.cpp
               "${PROJECT_SOURCE_DIR}/native/fbengine/jobs/jobs.cpp")
target_include_directories(fbengine_jobs_tests PRIVATE "${PROJECT_SOURCE_DIR}/native/fbengine")
target_link_libraries(fbengine_jobs_tests PRIVATE Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_jobs_tests)
catch_discover_tests(fbengine_jobs_tests TEST_PREFIX "jobs."
                     DL_PATHS "${fbengine_test_dll_paths}")

# res://'s paths, folders, and paks, built into the tests from their own sources.
add_executable(fbengine_vfs_tests vfs/vfs_tests.cpp
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/vfs.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/folder.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/pak.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/disk.cpp")
target_include_directories(fbengine_vfs_tests PRIVATE "${PROJECT_SOURCE_DIR}/native/fbengine")
target_link_libraries(fbengine_vfs_tests PRIVATE Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_vfs_tests)
catch_discover_tests(fbengine_vfs_tests TEST_PREFIX "vfs."
                     DL_PATHS "${fbengine_test_dll_paths}")

# A program that awaits jobs before its first Flip, which the main scheduler's ticks hand over.
if(FBENGINE_FB_TESTS)
  add_test(NAME examples.loading
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/loading")
  set_tests_properties(examples.loading PROPERTIES TIMEOUT 120
                       PASS_REGULAR_EXPRESSION "Running loading[^\n]*\nloading\r?\nloaded\r?\n$")
endif()

# The loaders' decoders, built into the tests from their own sources, with libktx's writer and
# encoder, which make the KTX2 files the tests read, and stb_image_write, the PNGs and JPEGs.
add_executable(fbengine_assets_tests assets/images_tests.cpp assets/models_tests.cpp
               assets/files.cpp
               "${PROJECT_SOURCE_DIR}/native/fbengine/assets/image.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/assets/gltf.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/assets/stb.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/assets/loads.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/vfs.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/folder.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/pak.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/disk.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/jobs/jobs.cpp")
fbengine_use_fetched_simdjson(fbengine_assets_tests)
target_include_directories(fbengine_assets_tests PRIVATE "${PROJECT_SOURCE_DIR}/native/fbengine")
# Hostile files the reviews found, which mustn't decode.
target_compile_definitions(fbengine_assets_tests PRIVATE
                           FBENGINE_ASSETS_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/assets/fixtures")
# stb_image_write's code, C as it's written, without fbengine's C++ warnings.
add_library(fbengine_stb_write STATIC assets/stb_write.c)
target_link_libraries(fbengine_stb_write PUBLIC fbengine_stb)
target_link_libraries(fbengine_assets_tests PRIVATE Catch2::Catch2WithMain fbengine_stb_write ktx
                      fastgltf::fastgltf)
fbengine_set_warnings(fbengine_assets_tests)
# libktx is UCRT's here, as the DLLs are (cmake/assets.cmake).
if(MINGW)
  target_link_options(fbengine_assets_tests PRIVATE -mcrtdll=ucrt)
endif()
catch_discover_tests(fbengine_assets_tests TEST_PREFIX "assets."
                     DL_PATHS "${fbengine_test_dll_paths}")

# What Assets' mounts panic for, each by a run of tests/assets/panics, which says it, at the line
# that called the mount.
if(FBENGINE_FB_TESTS)
  foreach(case IN ITEMS "files-empty;have '', which isn't a file's path"
                        "files-folder;have 'textures/', which isn't a file's path"
                        "files-above;have '\\.\\./x\\.txt', which isn't a file's path"
                        "files-colon;have 'a:b\\.txt', which isn't a file's path"
                        "files-device;have 'maps/con\\.txt', which isn't a file's path"
                        "files-file-and-folder;files mounted at res://mf can't be"
                        "files-place;data://mf"
                        "folder-place;goes above res://"
                        "model-material-high;MaterialName on a Model: it has 1 materials; there's no material 1"
                        "model-material-low;MaterialName on a Model: it has 1 materials; there's no material -1"
                        "model-texture-high;TextureName on a Model: it has 0 images; there's no image 0"
                        "model-texture-low;TextureName on a Model: it has 0 images; there's no image -1"
                        "model-texture-of-high;TextureNameOf on a Model: it has 1 materials; there's no material 1"
                        "model-texture-of-low;TextureNameOf on a Model: it has 1 materials; there's no material -1"
                        "model-map-high;TextureOf on a Model: it has 1 materials; there's no material 1"
                        "model-freed;MaterialName on a Model that was freed"
                        "model-freed-texture;TextureName on a Model that was freed"
                        "model-freed-texture-of;TextureNameOf on a Model that was freed")
    list(GET case 0 name)
    list(GET case 1 said)
    add_test(NAME assets.panics.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/assets/panics"
                     -- ${name})
    set_tests_properties(assets.panics.${name} PROPERTIES PASS_REGULAR_EXPRESSION
                         "panic: [^\n]*${said}[^\n]* at [^\n]*assets/panics/main\\.fb:"
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                         SKIP_REGULAR_EXPRESSION "no GPU here")
  endforeach()
endif()

# fbengine's app area apart from its DLL: what it shows nothing for, and what a link opens, on
# Windows read back through the shell; it opens nothing.
add_executable(fbengine_app_tests app/app_tests.cpp "${PROJECT_SOURCE_DIR}/native/fbengine/app/link.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/app/languages.cpp")
target_include_directories(fbengine_app_tests PRIVATE "${PROJECT_SOURCE_DIR}/native/fbengine")
target_link_libraries(fbengine_app_tests PRIVATE SDL3::SDL3-static Catch2::Catch2WithMain)
if(WIN32)
  target_link_libraries(fbengine_app_tests PRIVATE shlwapi)
endif()
fbengine_set_warnings(fbengine_app_tests)
catch_discover_tests(fbengine_app_tests TEST_PREFIX "app."
                     DL_PATHS "${fbengine_test_dll_paths}")

# fbengine_video, through its DLL, against its test videos with bytes changed at random.
add_executable(fbengine_video_tests video/mutation_tests.cpp)
target_link_libraries(fbengine_video_tests PRIVATE fbengine_video_native Catch2::Catch2WithMain)
target_compile_definitions(fbengine_video_tests PRIVATE
                           FBE_VIDEO_MEDIA="${PROJECT_SOURCE_DIR}/packages/fbengine_video/tests/media")
fbengine_set_warnings(fbengine_video_tests)
catch_discover_tests(fbengine_video_tests TEST_PREFIX "video."
                     DL_PATHS "${fbengine_test_dll_paths};$<TARGET_FILE_DIR:fbengine_video_native>")

# A video's thread, built in with a decoder of the test's, for what only its timing shows.
add_executable(fbengine_video_thread_tests video/thread_tests.cpp
               "${PROJECT_SOURCE_DIR}/native/fbengine_video/video/video.cpp")
target_include_directories(fbengine_video_thread_tests PRIVATE
                           "${PROJECT_SOURCE_DIR}/native/fbengine_video")
target_compile_definitions(fbengine_video_thread_tests PRIVATE FBE_VIDEO_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}")
target_link_libraries(fbengine_video_thread_tests PRIVATE Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_video_thread_tests)
catch_discover_tests(fbengine_video_thread_tests TEST_PREFIX "video."
                     DL_PATHS "${fbengine_test_dll_paths}")

# A program that opens a video and ends at once, its thread decoding, run again and again.
add_executable(fbengine_video_ends video/ends.cpp)
target_link_libraries(fbengine_video_ends PRIVATE fbengine_video_native)
target_compile_definitions(fbengine_video_ends PRIVATE
                           FBE_VIDEO_MEDIA="${PROJECT_SOURCE_DIR}/packages/fbengine_video/tests/media")
fbengine_set_warnings(fbengine_video_ends)
add_test(NAME video.ends
         COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:fbengine_video_ends>"
                 -DRUNS=150 "-DDLLS=$<TARGET_FILE_DIR:fbengine_video_native>;${fbengine_test_dll_paths}"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/physics/ends.cmake")
set_tests_properties(video.ends PROPERTIES TIMEOUT 600)

# What fbengine_video panics for, each by a run of tests/video/panics, which says it.
if(FBENGINE_FB_TESTS)
  foreach(case IN ITEMS "update-huge;Update: a video's clock moves on by 0 to a billion"
                        "update-huge-with-sound;Update: a video's clock moves on by 0 to a billion"
                        "volume-negative;SetVolume: a volume is 0 or more"
                        "volume-infinite;SetVolume: a volume is 0 or more"
                        "fit-nothing;Fit: a video's fitted into more than 0 by 0")
    list(GET case 0 name)
    list(GET case 1 said)
    add_test(NAME video.panics.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/video/panics"
                     -- ${name} "${PROJECT_SOURCE_DIR}/packages/fbengine_video/tests/media/colors.mpg")
    set_tests_properties(video.panics.${name} PROPERTIES PASS_REGULAR_EXPRESSION
                         "panic: Video\\.${said}" TIMEOUT 60)
  endforeach()
endif()
# E6 step 8: Khronos's sample models against what they show of themselves and their reference images,
# as tests/khronos/README.md says, fetched at a commit, each by its hash; and the frames it saves
# against their golden images. Offline, they're left out with a warning, unless
# FBENGINE_REQUIRE_KHRONOS, which CI sets, makes that fatal.
option(FBENGINE_REQUIRE_KHRONOS "Fail configuring if Khronos's sample models can't be fetched" OFF)
if(FBENGINE_FB_TESTS)
  include("${CMAKE_CURRENT_SOURCE_DIR}/khronos/fetch.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/khronos/files.cmake")
  set(fbengine_khronos "${PROJECT_BINARY_DIR}/khronos")
  set(fbengine_khronos_here TRUE)
  foreach(each IN LISTS fbengine_khronos_files)
    string(REPLACE "|" ";" each "${each}")
    list(GET each 0 from)
    list(GET each 1 to)
    list(GET each 2 hash)
    fbengine_fetch_khronos(
      "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/${fbengine_khronos_commit}/${from}"
      "${fbengine_khronos}/${to}" "${hash}" "${FBENGINE_REQUIRE_KHRONOS}" fetched)
    if(NOT fetched)
      set(fbengine_khronos_here FALSE)
      break()
    endif()
  endforeach()
  add_test(NAME khronos.fetch
           COMMAND "${CMAKE_COMMAND}" -DFOLDER=${PROJECT_BINARY_DIR}/khronos-fetch
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/khronos/fetch_test.cmake")
  set_tests_properties(khronos.fetch PROPERTIES
                       PASS_REGULAR_EXPRESSION "fetch's ways are as they should be")
  add_test(NAME khronos.fetch_wrong
           COMMAND "${CMAKE_COMMAND}" -DFOLDER=${PROJECT_BINARY_DIR}/khronos-fetch -DWRONG=1
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/khronos/fetch_test.cmake")
  set_tests_properties(khronos.fetch_wrong PROPERTIES
                       PASS_REGULAR_EXPRESSION "pinned")
  add_test(NAME khronos.fetch_never
           COMMAND "${CMAKE_COMMAND}" -DFOLDER=${PROJECT_BINARY_DIR}/khronos-fetch -DNEVER=1
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/khronos/fetch_test.cmake")
  set_tests_properties(khronos.fetch_never PROPERTIES
                       PASS_REGULAR_EXPRESSION "4 tries, couldn't fetch")
  if(fbengine_khronos_here)
    add_test(NAME khronos.models
             COMMAND "${FBENGINE_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/khronos"
                     -- --models "${fbengine_khronos}" --frames "${PROJECT_BINARY_DIR}/example-frames/khronos")
    set_tests_properties(khronos.models PROPERTIES
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                         FIXTURES_SETUP khronos_frames TIMEOUT 600
                         PASS_REGULAR_EXPRESSION "\nall [0-9]+ as expected\r?\n$"
                         SKIP_REGULAR_EXPRESSION "no GPU here")
    add_test(NAME khronos.golden
             COMMAND fbengine_gpu_tests "Khronos's sample models' frames match their golden images")
    set_tests_properties(khronos.golden PROPERTIES
                         LABELS "gpu;golden" FIXTURES_REQUIRED khronos_frames
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}")
    if(fbengine_test_dll_paths)
      set_property(TEST khronos.golden PROPERTY ENVIRONMENT_MODIFICATION
                   "PATH=path_list_prepend:${fbengine_test_dll_paths}")
    endif()
  endif()
endif()

# What fbengine_editor panics for, each by a run of its panics/main.fb, which says it.
if(FBENGINE_FB_TESTS)
  # Its panes against their golden images, its --save saving them first; and its --watch.
  add_test(NAME editor.frames
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/packages/fbengine_editor"
                   -- --save "${PROJECT_BINARY_DIR}/example-frames/editor")
  set_tests_properties(editor.frames PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       FIXTURES_SETUP editor_frames TIMEOUT 300
                       PASS_REGULAR_EXPRESSION "\nsaved\r?\n$"
                       SKIP_REGULAR_EXPRESSION "no GPU here")
  add_test(NAME editor.golden
           COMMAND fbengine_gpu_tests "the editor's frames match their golden images")
  set_tests_properties(editor.golden PROPERTIES
                       LABELS "gpu;golden" FIXTURES_REQUIRED editor_frames
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}")
  if(fbengine_test_dll_paths)
    set_property(TEST editor.golden PROPERTY ENVIRONMENT_MODIFICATION
                 "PATH=path_list_prepend:${fbengine_test_dll_paths}")
  endif()
  set(fbengine_editor_folder "${PROJECT_SOURCE_DIR}/packages/fbengine_editor")
  fbengine_add_watched(editor.watch "${fbengine_editor_folder}" "" "\nwatched 40 frames\r?\n$")
  fbengine_add_watched(editor.watch_escape "${fbengine_editor_folder}" "--press-at;1"
                       "\nwatched 2 frames\r?\n$")
  fbengine_add_watched(editor.watch_close "${fbengine_editor_folder}" "--close-at;1"
                       "\nwatched 2 frames\r?\n$")
  foreach(case IN ITEMS "mark-in-an-edit;History.MarkSaved: an edit can't make another"
                        "end-group-in-an-edit;History.EndGroup: an edit can't make another"
                        "move-nan;Dock.Move: a share is a number"
                        "history-none;History: it keeps more than 0 edits"
                        "commit-in-an-edit;History.Commit: an edit can't make another"
                        "undo-in-a-group;History.Undo: a group's begun"
                        "redo-in-a-group;History.Redo: a group's begun"
                        "end-no-group;History.EndGroup: no group's begun"
                        "command-twice;Commands.Add: there's a command a already"
                        "command-unknown;Commands.Run: there's no command a"
                        "pane-twice;Dock.AddPane: there's a pane a already"
                        "show-unknown;Dock.Show: there's no pane b"
                        "activate-no-tab;Dock.Activate: a has no tab"
                        "menu-entry-unknown;a menu's entry a isn't a command")
    list(GET case 0 name)
    list(GET case 1 said)
    add_test(NAME editor.panics.${name}
             COMMAND "${FBENGINE_FB}" --color never run
                     "${PROJECT_SOURCE_DIR}/packages/fbengine_editor/panics/main.fb" -- ${name})
    set_tests_properties(editor.panics.${name} PROPERTIES PASS_REGULAR_EXPRESSION "panic: ${said}" TIMEOUT 60)
  endforeach()
endif()
# text's atlas and layouts, as the canvas reads them, from its sources and core's, on HarfBuzz's
# test fonts.
file(GLOB fbengine_text_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/native/fbengine/text/*.cpp")
add_executable(fbengine_text_tests text/text_tests.cpp ${fbengine_text_sources}
               "${PROJECT_SOURCE_DIR}/native/fbengine/core/core.cpp")
target_include_directories(fbengine_text_tests PRIVATE "${PROJECT_SOURCE_DIR}/native/fbengine")
target_compile_definitions(fbengine_text_tests PRIVATE FBE_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}"
                           FBE_TEST_FONTS="${harfbuzz_SOURCE_DIR}/test/api/fonts"
                           FBE_OWN_TEST_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
                           FBE_SHAPE_TEST_FONTS="${harfbuzz_SOURCE_DIR}/test/shape/data/in-house/fonts")
target_link_libraries(fbengine_text_tests PRIVATE SDL3::SDL3-static freetype fbengine_harfbuzz
                      fbengine_sheenbidi Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_text_tests)
catch_discover_tests(fbengine_text_tests TEST_PREFIX "text."
                     DL_PATHS "${fbengine_test_dll_paths}")

# collide's masks against a brute force, from its sources and core's.
add_executable(fbengine_collide_tests collide/collide_tests.cpp
               "${PROJECT_SOURCE_DIR}/native/fbengine/collide/collide.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/core/core.cpp")
target_include_directories(fbengine_collide_tests PRIVATE "${PROJECT_SOURCE_DIR}/native/fbengine")
target_compile_definitions(fbengine_collide_tests PRIVATE FBE_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}")
target_link_libraries(fbengine_collide_tests PRIVATE SDL3::SDL3-static Catch2::Catch2WithMain)
# Its float arithmetic as written, never fused, as fbengine_native's is, so its corners and the
# GPU's it's checked against round alike on every target.
if(NOT MSVC)
  target_compile_options(fbengine_collide_tests PRIVATE -ffp-contract=off)
endif()
fbengine_set_warnings(fbengine_collide_tests)
catch_discover_tests(fbengine_collide_tests TEST_PREFIX "collide."
                     DL_PATHS "${fbengine_test_dll_paths}")

# fbengine_ui's panics, each by a run of tests/ui, which says it.
if(FBENGINE_FB_TESTS)
  set(fbengine_ui_cases
      "add-twice|panic: Node[.]Add: Node \"child\" is in another Node already"
      "add-into-itself|panic: Node[.]Add: Node \"outer\" can't go in itself"
      "insert-out-of-range|panic: Node[.]Insert: 1 isn't 0 to 0"
      "fixed-negative|panic: Sizing[.]Fixed: a length is 0 or more, not -1"
      "fill-zero|panic: Sizing[.]Fill: a weight is over 0, not 0"
      "fraction-past-one|panic: Sizing[.]Fraction: a fraction is 0 to 1, not 1[.]5"
      "style-empty-name|panic: Theme[.]SetFields: \"button[.]\" isn't a style's name"
      "set-width-unchecked|panic: Node[.]SetWidth: a fill's weight is over 0, not 0"
      "min-size-nan|panic: Node[.]SetMinSize: a least is 0 or more, and finite, not NaN"
      "padding-negative|panic: Node[.]SetPadding: padding is 0 or more, and finite, not -1"
      "margin-infinite|panic: Node[.]SetMargin: a margin is finite, not Inf"
      "anchors-backward|panic: Node[.]SetAnchors: a Max is at its Min or after"
      "style-negative-border|panic: Theme[.]SetFields: a style's widths, radii, and sizes are 0 or more, not -1"
      "style-nan-font-size|panic: Theme[.]SetFields: a style's widths, radii, and sizes are 0 or more, not NaN"
      "style-negative-padding|panic: Theme[.]SetFields: a style's padding is 0 or more, not -2"
      "max-size-negative|panic: Node[.]SetMaxSize: a most is 0 or more, or Inf, not -1 by 0"
      "gap-infinite|panic: Node[.]SetGap: a gap is 0 or more, and finite, not Inf"
      "content-size-nan|panic: Node[.]SetContentSize: a size is 0 or more, and finite, not NaN by 0"
      "anchor-nan|panic: Node[.]SetAnchors: an anchor is finite, not NaN"
      "slider-range-backward|panic: Slider[.]SetRange: a range is finite, and ends at its start or after, not 2 to 1"
      "slider-range-too-wide|panic: Slider[.]SetRange: a range is finite, and ends at its start or after, not -3"
      "slider-step-negative|panic: Slider[.]SetStep: a step is 0 or more, not -1"
      "max-length-negative|panic: TextField[.]SetMaxLength: a most is 0, for none, or more, not -1"
      "wheel-step-zero|panic: ScrollView[.]SetWheelStep: a step is over 0, not 0"
      "scale-zero|panic: Ui[.]SetScale: a scale is over 0, not 0"
      "command-with-a-space|panic: Console[.]Register: \"give item\" isn't a command's name"
      "console-no-lines|panic: Console[.]SetMaxLines: a console keeps 1 line or more, not 0"
      "debug-row-two-kinds|panic: DebugPanel[.]Switch: the debug row mode is a Label"
      "strings-fall-back-to-themselves|panic: Strings[.]SetFallback: en's strings fall back to fr's, which fall back to them")
  foreach(case IN LISTS fbengine_ui_cases)
    string(REPLACE "|" ";" case "${case}")
    list(GET case 0 name)
    list(GET case 1 printed)
    add_test(NAME ui.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/tests/ui" -- ${name})
    set_tests_properties(ui.${name} PROPERTIES PASS_REGULAR_EXPRESSION "${printed}" TIMEOUT 120)
  endforeach()
  # The IME's composition, set before the window opens, drawn at a field's cursor.
  add_test(NAME ui.composition
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/tests/ui" -- composition)
  set_tests_properties(ui.composition PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;FBENGINE_OWN_TEST_FONTS=${CMAKE_CURRENT_SOURCE_DIR}/fonts;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "underlined True, cursor True, typed a\r?\nhalf size, underlined True" TIMEOUT 120)
  add_test(NAME ui.composition-moved
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/tests/ui" -- composition-moved)
  set_tests_properties(ui.composition-moved PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "composing bb, then none" TIMEOUT 120)
  add_test(NAME ui.pop-clip-too-many
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/tests/ui" -- pop-clip-too-many)
  set_tests_properties(ui.pop-clip-too-many PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "Painter[.]PopClip: Node \"greedy\" ends a clip it didn't push" TIMEOUT 120)
endif()

# The cooker's C++, built into the tests from its own sources, which read back what it makes with
# libktx, which is UCRT's here, as the DLLs are, and with the engine's own glTF loader.
add_executable(fbengine_tools_tests tools/cook_tests.cpp tools/model_tests.cpp assets/files.cpp
               "${PROJECT_SOURCE_DIR}/native/fbengine_tools/cook/glb.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine_tools/cook/image.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine_tools/cook/model.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine_tools/cook/stb.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/assets/gltf.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/assets/image.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/jobs/jobs.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/disk.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/folder.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/pak.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/vfs.cpp"
               tools/pak_tests.cpp "${PROJECT_SOURCE_DIR}/native/fbengine_tools/cook/pak.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine_tools/tools/atomic.cpp")
fbengine_use_fetched_simdjson(fbengine_tools_tests)
target_include_directories(fbengine_tools_tests PRIVATE
                           "${PROJECT_SOURCE_DIR}/native/fbengine_tools"
                           "${PROJECT_SOURCE_DIR}/native/fbengine")
# basisu's job pool, which libktx's encoder is patched in (cmake/assets.cmake).
target_include_directories(fbengine_tools_tests SYSTEM PRIVATE
                           "${ktx_SOURCE_DIR}/external/basisu/encoder")
target_compile_definitions(fbengine_tools_tests PRIVATE
                           FBENGINE_ASSETS_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/assets/fixtures")
target_link_libraries(fbengine_tools_tests PRIVATE Catch2::Catch2WithMain fbengine_stb_write ktx
                      fastgltf::fastgltf meshoptimizer)
fbengine_set_warnings(fbengine_tools_tests)
if(MINGW)
  target_link_options(fbengine_tools_tests PRIVATE -mcrtdll=ucrt)
endif()
catch_discover_tests(fbengine_tools_tests TEST_PREFIX "tools."
                     DL_PATHS "${fbengine_test_dll_paths}")

# Typed references: a module tests/tools/references writes of its files/, which a game builds and
# prints from, and which a game that names a file that isn't there doesn't build with.
if(FBENGINE_FB_TESTS)
  set(fbengine_references_games "${CMAKE_CURRENT_BINARY_DIR}/references_games")
  add_test(NAME tools.references.write
           COMMAND "${FBENGINE_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/tools/references"
                   -- "${fbengine_references_games}"
                   "${CMAKE_CURRENT_SOURCE_DIR}/tools/references/files"
                   "${CMAKE_CURRENT_SOURCE_DIR}/tools/references/trees")
  set_tests_properties(tools.references.write PROPERTIES FIXTURES_SETUP fbengine_references
                       PASS_REGULAR_EXPRESSION "\nwritten\r?\n$")
  add_test(NAME tools.references.game
           COMMAND "${FBENGINE_FB}" --color never run "${fbengine_references_games}/game")
  set_tests_properties(tools.references.game PROPERTIES FIXTURES_REQUIRED fbengine_references
                       PASS_REGULAR_EXPRESSION
                       "\nres://logo\.png\r?\nres://Textures/wall\.png\r?\nres://Textures/Walls/stone\.png\r?\n$")
  add_test(NAME tools.references.typo
           COMMAND "${FBENGINE_FB}" --color never run "${fbengine_references_games}/typo")
  set_tests_properties(tools.references.typo PROPERTIES FIXTURES_REQUIRED fbengine_references
                       PASS_REGULAR_EXPRESSION "error: Textures has no member named 'Wal'")
  foreach(tree IN ITEMS case1 case3 m1 m2)
    add_test(NAME tools.references.tree.${tree}
             COMMAND "${FBENGINE_FB}" --color never run "${fbengine_references_games}/tree-${tree}")
    set_tests_properties(tools.references.tree.${tree} PROPERTIES
                         FIXTURES_REQUIRED fbengine_references PASS_REGULAR_EXPRESSION
                         "\nbuilt\r?\n$")
  endforeach()
endif()

# The job system's threads, started as the process ends, which mustn't hang its exit.
if(FBENGINE_FB_TESTS)
  add_test(NAME jobs.ends COMMAND "${FBENGINE_FB}" --color never run
           "${CMAKE_CURRENT_SOURCE_DIR}/jobs/ends")
  set_tests_properties(jobs.ends PROPERTIES TIMEOUT 180
                       PASS_REGULAR_EXPRESSION "
each ended
?
$")
endif()

# E8's done-when, a game whose build step is fbengine_tools', built each way, one at a time, as they
# share its build folder: its files as they are, one reloading as it changes, then cooked into its
# pak.
if(FBENGINE_FB_TESTS)
  # The cooker's threads, which can't hold up a whole run if they ever hang again.
  set_tests_properties(fbengine_tools.package PROPERTIES TIMEOUT 600)
  # 40 seconds here; one that hangs shouldn't take the job's hour with it.
  set_tests_properties(fbengine.package PROPERTIES TIMEOUT 600)
  # A copy, whose path dependencies are the packages', and whose file a run can change.
  set(fbengine_game "${CMAKE_CURRENT_BINARY_DIR}/cooked_game")
  set(fbengine_game_source "${PROJECT_SOURCE_DIR}/examples/cooked_game")
  file(READ "${fbengine_game_source}/project.toml" fbengine_game_project)
  string(REPLACE "../../packages" "${PROJECT_SOURCE_DIR}/packages" fbengine_game_project
                 "${fbengine_game_project}")
  configure_file("${fbengine_game_source}/main.fb" "${fbengine_game}/main.fb" COPYONLY)
  file(COPY "${fbengine_game_source}/assets" DESTINATION "${fbengine_game}")
  file(CONFIGURE OUTPUT "${fbengine_game}/project.toml" CONTENT "${fbengine_game_project}" @ONLY)
  # Intel Macs have no Vulkan to draw with, which the game says, in place of what it drew.
  set(fbengine_game_red "drew red\r?\ndrew a green crate\r?\n")
  set(fbengine_game_blue "hero reloaded, 8 wide\r?\ndrew blue\r?\n")
  if(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    set(fbengine_game_red "no GPU[^\n]*\n")
    set(fbengine_game_blue "")
  endif()
  # The debug run writes over its hero, which each run starts without.
  add_test(NAME examples.cooked_game.hero
           COMMAND "${CMAKE_COMMAND}" -E copy "${fbengine_game_source}/assets/hero.png"
                   "${fbengine_game}/assets/hero.png")
  set_tests_properties(examples.cooked_game.hero PROPERTIES RESOURCE_LOCK fbengine_game
                       FIXTURES_SETUP fbengine_game_hero)
  add_test(NAME examples.cooked_game.debug
           COMMAND "${FBENGINE_FB}" --color never run "${fbengine_game}" -- --change
                   "${fbengine_game}/assets/hero.png")
  set_tests_properties(examples.cooked_game.debug PROPERTIES RESOURCE_LOCK fbengine_game TIMEOUT 300
                       FIXTURES_REQUIRED fbengine_game_hero
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION
                       "\nlevel one\r?\nhero, as it is\r?\n${fbengine_game_red}${fbengine_game_blue}$")
  add_test(NAME examples.cooked_game.watch
           COMMAND "${FBENGINE_FB}" --color never run "${fbengine_game}" -- --watch --frames 3)
  set_tests_properties(examples.cooked_game.watch PROPERTIES RESOURCE_LOCK fbengine_game TIMEOUT 300
                       FIXTURES_REQUIRED fbengine_game_hero
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "\nhero, as it is\r?\n[^\n]*\n$")
  add_test(NAME examples.cooked_game.release
           COMMAND "${FBENGINE_FB}" --color never run --release "${fbengine_game}")
  set_tests_properties(examples.cooked_game.release PROPERTIES RESOURCE_LOCK fbengine_game
                       TIMEOUT 300 FIXTURES_REQUIRED fbengine_game_hero
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION
                       "\nlevel one\r?\nhero, cooked\r?\n${fbengine_game_red}$")
  # One without its assets/, whose step fails, saying so.
  set(fbengine_game_bare "${CMAKE_CURRENT_BINARY_DIR}/cooked_game_bare")
  configure_file("${fbengine_game_source}/main.fb" "${fbengine_game_bare}/main.fb" COPYONLY)
  file(CONFIGURE OUTPUT "${fbengine_game_bare}/project.toml" CONTENT "${fbengine_game_project}" @ONLY)
  add_test(NAME tools.game.bare COMMAND "${FBENGINE_FB}" --color never build "${fbengine_game_bare}")
  set_tests_properties(tools.game.bare PROPERTIES PASS_REGULAR_EXPRESSION
                       "assets isn't there: it holds what cooked_game has at res://")
endif()

# fbengine_audio's own C++, where it's subtle: what's in its headers, and miniaudio's backends.
add_executable(fbengine_audio_tests audio/pool_tests.cpp audio/backends_tests.cpp
               audio/channels_tests.cpp audio/outputs_tests.cpp)
target_include_directories(fbengine_audio_tests PRIVATE "${PROJECT_SOURCE_DIR}/native/fbengine_audio")
target_link_libraries(fbengine_audio_tests PRIVATE fbengine_miniaudio Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_audio_tests)
catch_discover_tests(fbengine_audio_tests TEST_PREFIX "audio."
                     DL_PATHS "${fbengine_test_dll_paths}")

# fbengine_audio_native's C API, with a thread of the test's own mixing, as a device's does, and on
# Windows, with OLE, on the null backend.
add_executable(fbengine_audio_native_tests audio/native_tests.cpp audio/memory_tests.cpp)
target_link_libraries(fbengine_audio_native_tests PRIVATE fbengine_audio_native
                      Catch2::Catch2WithMain)
target_compile_definitions(fbengine_audio_native_tests PRIVATE
                           FBE_AUDIO_TEST_DATA="${PROJECT_SOURCE_DIR}/packages/fbengine_audio/tests/data")
if(WIN32)
  target_sources(fbengine_audio_native_tests PRIVATE audio/ole_tests.cpp)
  target_link_libraries(fbengine_audio_native_tests PRIVATE ole32 psapi)
endif()
fbengine_set_warnings(fbengine_audio_native_tests)
# A test tagged [stability], a fuzz, race, or soak, is labeled so, for sanitizers.yml.
catch_discover_tests(fbengine_audio_native_tests TEST_PREFIX "audio." ADD_TAGS_AS_LABELS
                     DL_PATHS "${fbengine_test_dll_paths};$<TARGET_FILE_DIR:fbengine_audio_native>")

# fbengine_audio_native's C API with each of a call's allocations failing in turn: its sources built
# into the test, whose operator new fails the one it says.
file(GLOB fbengine_audio_oom_sources CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/native/fbengine_audio/*/*.cpp")
add_executable(fbengine_audio_oom_tests audio/oom_tests.cpp ${fbengine_audio_oom_sources})
target_include_directories(fbengine_audio_oom_tests PRIVATE
                           "${PROJECT_SOURCE_DIR}/native/fbengine_audio")
target_compile_definitions(fbengine_audio_oom_tests PRIVATE FBE_AUDIO_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}")
target_link_libraries(fbengine_audio_oom_tests PRIVATE fbengine_miniaudio Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_audio_oom_tests)
catch_discover_tests(fbengine_audio_oom_tests TEST_PREFIX "audio."
                     DL_PATHS "${fbengine_test_dll_paths}")

# Playing a sound to miniaudio's null backend, as a game plays to its device; and ending with the
# audio started, as a program does and as a panic does, which ends at once, never waiting on its
# device's thread.
if(FBENGINE_FB_TESTS)
  add_test(NAME examples.sound
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/sound")
  set_tests_properties(examples.sound PROPERTIES TIMEOUT 60
                       PASS_REGULAR_EXPRESSION "Running sound[^\n]*\nplayed 0.25 seconds of tone.ogg\r?\n$")
  add_test(NAME examples.audio_exit
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/audio_exit")
  set_tests_properties(examples.audio_exit PROPERTIES TIMEOUT 60
                       PASS_REGULAR_EXPRESSION "Running audio_exit[^\n]*\nending with the audio started\r?\n$")
  add_test(NAME examples.audio_panic
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/audio_panic")
  set_tests_properties(examples.audio_panic PROPERTIES TIMEOUT 60
                       PASS_REGULAR_EXPRESSION "panic: a mistake, with the audio started at [^\n]*\n.*exited with code 101")
  # Section 6's item 15, by numbers, on no device and the null backend; --watch, for a person to
  # hear, keeps to the null backend under --frames.
  set(fbengine_sound_proof
      "a Channel of a held 0.5: 0.5, 0.5"
      "at half its volume: 0.25, 0.25"
      "panned left: 0.5, 0"
      "panned right: 0, 0.5"
      "at pitch 2, a tenth of a second plays 0.2 of it"
      "at 22050 Hz, a tenth of a second plays 0.05 of it"
      "paused: 0, 0, playing True, paused True, held where it was True"
      "resumed: 0.5, 0.5, on from there True"
      "stopped: 0, 0, playing False"
      "a finished Channel's volume, set: 0"
      "64 at once at 44100 Hz: 64 playing, heard 0.5, 0.5"
      "Ogg Vorbis decoded: streamed False, 0.25 seconds"
      "heard True, then over True"
      "Ogg Vorbis streamed: streamed True, 0.25 seconds"
      "after twice its length: playing True, come round True, heard True"
      "a stream of a held 0.5: 0.5, 0.5"
      "at half its volume: 0.25, 0.25"
      "panned left: 0.5, 0"
      "paused: 0, 0, held where it was True"
      "resumed: 0.5, 0.5"
      "the first piece: 0.5, 0.5"
      "half way through the crossfade: 0.375, 0.375"
      "after it: 0.25, 0.25, the first playing False, the second True"
      "following a car 1 ahead: 0.25, 0.25"
      "2 ahead, twice as far: 0.125, 0.125"
      "to the right: heard on the right"
      "the camera turned toward it: heard ahead"
      "the camera turned round: heard on the left"
      "the car coming: moving toward the camera True"
      "at half the speed of sound, a tenth of a second plays 0.2 of it"
      "the car freed: following False, still heard True"
      "on the null backend: played to its end True, frames mixed True"
      )
  list(JOIN fbengine_sound_proof "\r?\n" fbengine_sound_proof_lines)
  add_test(NAME proof.streamed_and_positional_sound
           COMMAND "${FBENGINE_FB}" --color never run
                   "${PROJECT_SOURCE_DIR}/examples/proof/streamed_and_positional_sound")
  set_tests_properties(proof.streamed_and_positional_sound PROPERTIES TIMEOUT 120
                       PASS_REGULAR_EXPRESSION "\n${fbengine_sound_proof_lines}\r?\n$")
  # Section 6's item 13, by numbers and drawn into a texture, and its --watch with no window.
  set(fbengine_skin_proof
      "bound, its hand at 1.7, 1.3, 0"
      "at frame 7.5, at 30 a second, its arm's 22.5 up"
      "at its end, its hand at 0.3, 2.7, 0"
      "half way through a crossfade, its arm's 15 up"
      "after it, playing stand, its arm's -60 up"
      "its face, before, at 0, 1.6, -1"
      "its head turned, looking along 1, 0, 0, its face at 1, 1.6, 0"
      "after an update, its head still looks along 1, 0, 0"
      "its arm turned by the game: 45 up"
      "posed again by an update: -60 up"
      "released, it stays: 45 up"
      "reclaimed, it's posed again: -60 up"
      "drawn raised: red 255 at its hand, 0 where it was"
      "drawn bound: red 255 at its hand, 0 where it went"
      )
  list(JOIN fbengine_skin_proof "\r?\n" fbengine_skin_proof_lines)
  add_test(NAME proof.skinned_animation_with_bones_under_game_control
           COMMAND "${FBENGINE_FB}" --color never run
                   "${PROJECT_SOURCE_DIR}/examples/proof/skinned_animation_with_bones_under_game_control")
  set_tests_properties(proof.skinned_animation_with_bones_under_game_control PROPERTIES TIMEOUT 120
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "
${fbengine_skin_proof_lines}\r?\n$")
  add_test(NAME proof.skinned_animation_with_bones_under_game_control_watch
           COMMAND "${FBENGINE_FB}" --color never run
                   "${PROJECT_SOURCE_DIR}/examples/proof/skinned_animation_with_bones_under_game_control"
                   -- --watch --frames 300)
  set_tests_properties(proof.skinned_animation_with_bones_under_game_control_watch PROPERTIES
                       TIMEOUT 120
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "
its head looks at the fly True, its body drawn True\r?\nwatched 300 frames\r?\n$")
  add_test(NAME proof.streamed_and_positional_sound_watch
           COMMAND "${FBENGINE_FB}" --color never run
                   "${PROJECT_SOURCE_DIR}/examples/proof/streamed_and_positional_sound"
                   -- --watch --frames 960)
  set_tests_properties(proof.streamed_and_positional_sound_watch PROPERTIES TIMEOUT 120
                       PASS_REGULAR_EXPRESSION "\npaused True, resumed True, the next playing True, the car's playing True, looping True\r?\nwatched 960 frames\r?\n$")
  # 64 channels at 44100 Hz under a game's load, within the budget.
  add_test(NAME examples.audio_stress
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/audio_stress")
  # What it holds the game's calls to still fails what really is slow, each way, every time.
  add_test(NAME examples.audio_stress_stalled_every_frame
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/audio_stress"
                   -- --stall-every-frame)
  set_tests_properties(examples.audio_stress_stalled_every_frame PROPERTIES TIMEOUT 240
                       PASS_REGULAR_EXPRESSION "run 3: the game's calls take more than audio's 0[.]5 ms a frame\r?\n.*panic: the game's calls take more than")
  add_test(NAME examples.audio_stress_stalled_now_and_then
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/audio_stress"
                   -- --stall-now-and-then)
  set_tests_properties(examples.audio_stress_stalled_now_and_then PROPERTIES TIMEOUT 240
                       PASS_REGULAR_EXPRESSION "run 3: the game's calls stalled in 60 of 600 frames\r?\n.*panic: the game's calls stalled")
  set_tests_properties(examples.audio_stress PROPERTIES TIMEOUT 240
                       PASS_REGULAR_EXPRESSION "600 frames on no device, up to 64 channels at once\r?\n.*within the budget\r?\n$")
  # What fbengine_audio panics for with fbengine's entities, each by a run of tests/audio/panics.
  foreach(case IN ITEMS "emitter-follows-freed;an Emitter can't follow a freed Pivot"
                        "listener-follows-freed;the Listener can't follow a freed Pivot")
    list(GET case 0 name)
    list(GET case 1 said)
    add_test(NAME audio.panics.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/audio/panics"
                     -- ${name})
    set_tests_properties(audio.panics.${name} PROPERTIES TIMEOUT 60
                         PASS_REGULAR_EXPRESSION "panic: ${said}")
  endforeach()
endif()

# fbengine_native's frame, each test in a process of its own, as a program's first frames are.
add_executable(fbengine_core_tests core/frame_tests.cpp)
target_link_libraries(fbengine_core_tests PRIVATE fbengine_native Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_core_tests)
catch_discover_tests(fbengine_core_tests TEST_PREFIX "core."
                     DL_PATHS "${fbengine_test_dll_paths};$<TARGET_FILE_DIR:fbengine_native>")

# E7's animation: sampling, apart from the DLL, and the clip, player, and skin areas through
# fbengine's C API, with no window or GPU.
add_executable(fbengine_anim_tests anim/sample_tests.cpp anim/clip_tests.cpp anim/player_tests.cpp
               anim/skin_tests.cpp anim/model_tests.cpp anim/morph_tests.cpp anim/fuzz_tests.cpp
               "${PROJECT_SOURCE_DIR}/native/fbengine/clip/sample.cpp")
target_link_libraries(fbengine_anim_tests PRIVATE fbengine_native Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_anim_tests)
catch_discover_tests(fbengine_anim_tests TEST_PREFIX "anim."
                     DL_PATHS "${fbengine_test_dll_paths}" "${PROJECT_BINARY_DIR}/native/fbengine")

# Animation's C API with each of a call's allocations failing in turn, with fbengine's sources, as
# operator new is this test's.
add_executable(fbengine_effects_oom_tests scene/effects_oom_tests.cpp)
target_link_libraries(fbengine_effects_oom_tests PRIVATE fbengine_internal Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_effects_oom_tests)
catch_discover_tests(fbengine_effects_oom_tests TEST_PREFIX "scene." ADD_TAGS_AS_LABELS
                     DL_PATHS "${fbengine_test_dll_paths}"
                     PROPERTIES ENVIRONMENT "${fbengine_vulkan_environment}")

add_executable(fbengine_anim_oom_tests anim/oom_tests.cpp)
target_link_libraries(fbengine_anim_oom_tests PRIVATE fbengine_internal Catch2::Catch2WithMain
                      ${CMAKE_DL_LIBS})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(fbengine_anim_oom_tests PRIVATE
                      "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/anim/own_new.map")
  set_property(TARGET fbengine_anim_oom_tests APPEND PROPERTY LINK_DEPENDS
               "${CMAKE_CURRENT_SOURCE_DIR}/anim/own_new.map")
elseif(APPLE)
  target_link_options(fbengine_anim_oom_tests PRIVATE
                      "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/anim/own_new.exports")
  set_property(TARGET fbengine_anim_oom_tests APPEND PROPERTY LINK_DEPENDS
               "${CMAKE_CURRENT_SOURCE_DIR}/anim/own_new.exports")
endif()
fbengine_set_warnings(fbengine_anim_oom_tests)
# A deadlock is a failure too, as an allocation failing inside a layer's lock was.
catch_discover_tests(fbengine_anim_oom_tests TEST_PREFIX "anim." ADD_TAGS_AS_LABELS
                     DL_PATHS "${fbengine_test_dll_paths}"
                     PROPERTIES ENVIRONMENT "${fbengine_vulkan_environment}" TIMEOUT 120)

# A program of the whole of it, as a game would write it.
if(FBENGINE_FB_TESTS)
  add_test(NAME examples.animation
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/animation")
  set_tests_properties(examples.animation PROPERTIES PASS_REGULAR_EXPRESSION
                       "Running animation[^\n]*\nstep in walk\r?\nwalked to 1.5\r?\nwaving at 0.5\r?\nlooking ahead\r?\nstep in walk\r?\nstopped: still\r?\n$")
endif()

# Animation's panics, each a case of tests/anim/panics run on its own, which has to panic saying
# what it does.
if(FBENGINE_FB_TESTS)
  set(fbengine_anim_panics
      "frame-rate=Clip: a clip's frames a second are 0 or more, not -1"
      "unpaired=AddPositions on a Clip: a key's time for each value, not 2 for 1"
      "untangented=AddRotations on a Clip: a CUBIC channel's keys have tangents"
      "tangents-unpaired=AddScales on a Clip: a key's time and tangents for each value"
      "times-falling=AddPositions on a Clip: each key's time is after the last: 1 isn't after 1"
      "moved-twice=AddScales on a Clip: the clip moves the scale of \"a\" already"
      "clip-freed=Length on a Clip that was freed"
      "player-of-freed=Player on a Pivot that was freed"
      "player-entity-freed=SetTime on a Player whose entity was freed"
      "time-nan=SetTime on a Player: a clip's time is a number of seconds, not nan"
      "frame-no-clip=SetFrame on a Player with no clip playing"
      "frame-no-rate=SetFrame on a Player: its clip \"no rate\" has no frames a second"
      "update-negative=Update on a World: a world is moved on by 0 seconds or more, not -1"
      "palette-unskinned=Palette on a Mesh skinned by nothing"
      "joint-twice=AddJoint on a Skin: the skin has a joint \"a\" already"
      "joint-out-of-range=JointName\\(0\\) on a Skin with 0 joints"
      "release-freed=Release on a Pivot that was freed"
      "weights-uneven=AddWeights on a Clip: as many weights for each key's time, not 3 for 2"
      "target-short=AddMorphTarget on a Geometry: a target moves each of the surface's 0 vertices, not 1"
      "morphed-nothing=MorphedPositions on a Mesh that draws nothing"
      "layer-out-of-range=Layer\\(16\\) on a Player: its layers are 0 to 15"
      "crossfade-negative=Crossfade on a Player: a fade takes 0 seconds or more, not -1"
      "weight-outside=SetWeight on a Player: a layer's weight is from 0 to 1, not 2"
      "event-out-of-range=EventName\\(0\\) on a Clip with 0 events"
      "slice-backward=Slice on a Clip: a clip's part is from 0 seconds or more to then or later, not 2 to 1")
  foreach(case IN LISTS fbengine_anim_panics)
    string(FIND "${case}" "=" split)
    string(SUBSTRING "${case}" 0 ${split} name)
    math(EXPR split "${split} + 1")
    string(SUBSTRING "${case}" ${split} -1 message)
    add_test(NAME anim.panics.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/anim/panics"
                     -- ${name})
    set_tests_properties(anim.panics.${name} PROPERTIES TIMEOUT 60
                         PASS_REGULAR_EXPRESSION "panic: ${message}")
  endforeach()
endif()

# A test of a panic that hangs at the program's end, as one with the GPU going can on Windows, fails
# in a minute, not ctest's 25, which all together would take the job's hour. One that says its own
# limit keeps it.
get_property(fbengine_all_tests DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS fbengine_all_tests)
  get_test_property("${test}" PASS_REGULAR_EXPRESSION said)
  get_test_property("${test}" TIMEOUT limit)
  if(said MATCHES "panic" AND NOT limit)
    set_tests_properties("${test}" PROPERTIES TIMEOUT 60)
  endif()
endforeach()

# Every test sees only the tests' Vulkan driver, whether or not it means to draw: with none set, the
# loader offers the machine's real GPUs, and a run on the user's integrated GPU once took their PC
# down. verify.py reads the same driver from this file, for tests found at build time.
get_property(fbengine_all_tests DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS fbengine_all_tests)
  get_test_property("${test}" ENVIRONMENT said)
  if(NOT said MATCHES "VK_DRIVER_FILES=" AND NOT fbengine_vulkan_environment STREQUAL "")
    set_property(TEST "${test}" APPEND PROPERTY ENVIRONMENT "${fbengine_vulkan_environment}")
  endif()
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/tests-vulkan-driver.txt" "${fbengine_vulkan_driver}")
