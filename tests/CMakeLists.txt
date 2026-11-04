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
target_include_directories(fbengine_gpu_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
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
                             gpu/shaders/spheres.slang)
catch_discover_tests(fbengine_gpu_tests TEST_PREFIX "gpu." ADD_TAGS_AS_LABELS
                     DL_PATHS "${fbengine_test_dll_paths}"
                     PROPERTIES ENVIRONMENT "${fbengine_vulkan_environment}")

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
                     LABELS "gpu"
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

  # Every package's own tests, with SDL's offscreen video, so no test opens a window, as each test
  # asks too.
  file(GLOB fbengine_packages CONFIGURE_DEPENDS LIST_DIRECTORIES true
       "${PROJECT_SOURCE_DIR}/packages/*")
  foreach(package IN LISTS fbengine_packages)
    if(EXISTS "${package}/project.toml")
      cmake_path(GET package FILENAME name)
      add_test(NAME ${name}.package COMMAND "${FBENGINE_FB}" test "${package}")
      set_tests_properties(${name}.package PROPERTIES ENVIRONMENT
                           "SDL_VIDEO_DRIVER=offscreen;FBENGINE_TEST_FONTS=${harfbuzz_SOURCE_DIR}/test/api/fonts;FBENGINE_OWN_TEST_FONTS=${CMAKE_CURRENT_SOURCE_DIR}/fonts")
    endif()
  endforeach()

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
             COMMAND "${FBENGINE_FB}" --color never run "${folder}" -- --watch --frames 40 ${driving})
    set_tests_properties(${test} PROPERTIES
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                         PASS_REGULAR_EXPRESSION "${printed}"
                         SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 120)
  endfunction()
  function(fbengine_add_proof_of_numbers name)
    list(JOIN ARGN "\r?\n" printed)
    add_test(NAME proof.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/proof/${name}")
    set_tests_properties(proof.${name} PROPERTIES
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;FBENGINE_OWN_TEST_FONTS=${CMAKE_CURRENT_SOURCE_DIR}/fonts;${fbengine_vulkan_environment}"
                         PASS_REGULAR_EXPRESSION "\n${printed}\r?\n$"
                         SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 120)
  endfunction()
  function(fbengine_add_proof name)
    fbengine_add_proof_of_numbers(${name} ${ARGN})
    set(folder "${PROJECT_SOURCE_DIR}/examples/proof/${name}")
    add_test(NAME proof.${name}_watch
             COMMAND "${FBENGINE_FB}" --color never run "${folder}" -- --watch --frames 40)
    set_tests_properties(proof.${name}_watch PROPERTIES
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                         PASS_REGULAR_EXPRESSION "\nwatched 40 frames\r?\n$"
                         SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 120)
    fbengine_add_watched(proof.${name}_escape "${folder}" "--press-at;1" "\nwatched 2 frames\r?\n$")
    fbengine_add_watched(proof.${name}_close "${folder}" "--close-at;1" "\nwatched 2 frames\r?\n$")
  endfunction()
  function(fbengine_add_drawn_proof name)
    fbengine_add_proof(${name} ${ARGN})
    set_tests_properties(proof.${name}_watch PROPERTIES
                         PASS_REGULAR_EXPRESSION "\ndrew True\r?\nwatched 40 frames\r?\n$")
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
                     "a strip of 4 frames"
                     "frame 0 [(]255, 0, 0, 255[)]"
                     "frame 1 [(]0, 255, 0, 255[)]"
                     "frame 2 [(]0, 0, 255, 255[)]"
                     "frame 3 [(]255, 255, 255, 255[)]"
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
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                         TIMEOUT 300)
  endforeach()
  # And on the island, walked in 3D: to each ending, then the road as it floods.
  foreach(name IN ITEMS bright dim dark cut swept wing far_end)
    add_test(NAME examples.slack_water.walk3d_${name}
             COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/slack_water"
                     -- --walk3d ${name}
                     "${PROJECT_SOURCE_DIR}/examples/slack_water/tests/walk3d_${name}.txt")
    set_tests_properties(examples.slack_water.walk3d_${name} PROPERTIES RESOURCE_LOCK slack_water
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                         SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 300)
  endforeach()
  add_test(NAME examples.slack_water.end_screen
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/slack_water"
                   -- --end "${PROJECT_BINARY_DIR}/example-frames/slack_water")
  set_tests_properties(examples.slack_water.end_screen PROPERTIES RESOURCE_LOCK slack_water
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 300)
  # As it ships: a release, from its cooked pak.
  add_test(NAME examples.slack_water.cooked
           COMMAND "${FBENGINE_FB}" --color never run --release
                   "${PROJECT_SOURCE_DIR}/examples/slack_water"
                   -- --cooked "${PROJECT_SOURCE_DIR}/examples/slack_water/tests/walk3d_bright.txt"
                   "${PROJECT_BINARY_DIR}/example-frames/slack_water_cooked")
  set_tests_properties(examples.slack_water.cooked PROPERTIES RESOURCE_LOCK slack_water
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 600)
  add_test(NAME examples.slack_water.hear
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/slack_water"
                   -- --hear)
  set_tests_properties(examples.slack_water.hear PROPERTIES RESOURCE_LOCK slack_water
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 300)
  add_test(NAME examples.slack_water.feel
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/slack_water"
                   -- --feel)
  set_tests_properties(examples.slack_water.feel PROPERTIES RESOURCE_LOCK slack_water
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
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
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 300)
  # Its assets: what its tools make, here, and what's kept in the repository, each loaded as the game
  # loads them, and the two the same.
  add_test(NAME examples.slack_water.tools
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/slack_water/tools"
                   -- "${PROJECT_BINARY_DIR}/example-frames/slack_water_made")
  set_tests_properties(examples.slack_water.tools PROPERTIES
                       FIXTURES_SETUP slack_water_made TIMEOUT 300
                       PASS_REGULAR_EXPRESSION "15 textures\r?\n13 sounds\r?\n8 joints in Pip\r?\n$")
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
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                         SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 300)
  endforeach()
  add_test(NAME examples.slack_water.assets_match
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/slack_water"
                   -- --match "${PROJECT_BINARY_DIR}/example-frames/slack_water_made"
                   "${PROJECT_SOURCE_DIR}/examples/slack_water/assets")
  set_tests_properties(examples.slack_water.assets_match PROPERTIES RESOURCE_LOCK slack_water
                       FIXTURES_REQUIRED slack_water_made
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen" TIMEOUT 300)
  foreach(name IN ITEMS sprites_over_a_scene pixels_of_images_and_textures
          textures_by_their_settings)
    add_test(NAME proof.${name}_no_gpu
             COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/proof/${name}"
                     -- --watch --frames 2)
    set_tests_properties(proof.${name}_no_gpu PROPERTIES
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;FBENGINE_NO_GPU=1"
                         PASS_REGULAR_EXPRESSION "\nno GPU here\r?\n$" TIMEOUT 120)
  endforeach()

  # Those that draw: what each prints, and its --watch, which has to have drawn something.
  fbengine_add_drawn_proof(swept_collisions
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
                           "a mover into one moving away: -40[.]000, 1[.]000, -1[.]999, the other -40[.]000, 1[.]000, -13[.]000"
                           "a tall one onto a slope of 30: facing -0[.]500, 0[.]866, 0[.]000"
                           "a wall, then a rock: touching 2, level True, other True"
                           "a door moved across the move, this frame: 0[.]000, 1[.]000, -3[.]999"
                           "by UpdateWorld: 0[.]000, 0[.]501, 0[.]000, touching 1"
                           "into a box: 138[.]499, 1[.]000, 0[.]000")
  fbengine_add_drawn_proof(picks
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
  fbengine_add_drawn_proof(in_view_by_bounds
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
                           "past a box told to block sight False")
  fbengine_add_drawn_proof(scene_trees
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
                           "alpha 0: a mover rests at 0[.]000, 1[.]501, 0[.]000")
  fbengine_add_drawn_proof(meshes_in_code
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
  fbengine_add_drawn_proof(binary_saves_and_maps
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
  fbengine_add_drawn_proof(pathfinding_over_a_graph
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
  fbengine_add_drawn_proof(particles_on_sprites
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

  fbengine_add_drawn_proof(fixed_function_look
                           "a room's lightmap and diffuse, left: [(]125, 110, 102, 255[)]"
                           "and right, its lightmap's UVs its own: [(]75, 66, 61, 255[)]"
                           "a stage's clamped: [(]120, 120, 120, 255[)]"
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
                           "a sprite BILLBOARD, 32 across"
                           "a sprite FIXED, [56] across"
                           "a sprite UPRIGHT, 32 across")
  fbengine_add_drawn_proof(cameras_and_viewports
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
                           "a hidden camera's part stays: [(]0, 0, 255, 255[)]"
                           "the other's changes: [(]255, 0, 255, 255[)]")
  fbengine_add_drawn_proof(camera_into_a_texture
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
                           "over itself: [(]200, 100, 50, 255[)]")
  fbengine_add_drawn_proof(bump
                           "a flat map's left: [(]255, 0, 0, 255[)]"
                           "its right: [(]0, 255, 0, 255[)]"
                           "shifted a quarter across: [(]0, 255, 0, 255[)]"
                           "by its green: [(]0, 255, 0, 255[)]"
                           "by M01, down: [(]0, 0, 255, 255[)]"
                           "half its light, by its blue: [(]0, 128, 0, 255[)]"
                           "all of it, offset by half: [(]0, 255, 0, 255[)]"
                           "its matrix moves a texel or so back True")
  # A Mac's samplers have no LOD bias.
  if(APPLE)
    set(fbengine_lod_bias False)
  else()
    set(fbengine_lod_bias True)
  endif()
  fbengine_add_drawn_proof(render_settings
                           "by itself, its pixels are within 8 of each other True"
                           "the bias -3"
                           "sharper, they're apart by over 64 ${fbengine_lod_bias}"
                           "blurrier, within 8 True"
                           "1 sample, between 0"
                           "4 samples 4, between over 10 True"
                           "the screen's 4"
                           "on the screen, between over 10 True"
                           "filled 576"
                           "wireframe's lines, some True, under half True"
                           "inside a triangle: [(]0, 0, 0, 255[)]"
                           "validation errors 0")
  fbengine_add_drawn_proof(gamma
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
                           "and the other: [(]255, 255, 255, 255[)]")
  # Their --watch's own keys, each pressed by the walk.
  set_tests_properties(proof.render_settings_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nby its keys: bias 0, samples 1, wireframe True\r?\ndrew True\r?\nwatched 40 frames\r?\n$")
  set_tests_properties(proof.gamma_watch PROPERTIES
                       PASS_REGULAR_EXPRESSION "\nby its keys: gamma 1[.]25\r?\ndrew True\r?\nwatched 40 frames\r?\n$")

  # Their --watch, which a person watches in a window (tools/show.py), walked here offscreen, a step
  # each frame.
  add_test(NAME examples.controls_watch
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/controls"
                   -- --watch --frames 30)
  set_tests_properties(examples.controls_watch PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "\nwatched 30 frames, moved True, jumped True\r?\n$")
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
               scene/terrain_tests.cpp scene/clusters_tests.cpp
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
      "render-no-target=Render on a World: a camera with no target draws into the screen, and there's no window open"
      "render-bad-tween=Render on a World: a tween is 0 to 1, not 2"
      "layer-uv-zero=SetLayerUv on a Material: a layer's UV scale is finite and not 0, not \\(0, 1\\)"
      "layer-texture-out-of-range=SetTexture on a Material: a material's layers are 0 to 7, not 8"
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
      "intensity=SetIntensity on a Light: a light's intensity is 0 or more, not -1"
      "light-range=SetRange on a Light: a light's range is above 0, not 0"
      "cone=SetCone on a Light: a spot light's cone is 0 <= inner <= outer <= Pi / 2"
      "fog-range=SetFogRange on a Camera: a fog's range is 0 <= start < end, finite, not 5 to 5"
      "fog-density=SetFogDensity on a Camera: a fog's density is 0 or more"
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
      "layer=SetLayer on a Material: a material's layers are 0 to 7, not 8"
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
  # E14's proofs of this lane's: what each prints, line by line, and its --watch walked offscreen.
  function(fbengine_add_lane_proof name)
    list(JOIN ARGN "
?
" printed)
    set(folder "${PROJECT_SOURCE_DIR}/examples/proof/${name}")
    add_test(NAME proof.${name} COMMAND "${FBENGINE_FB}" --color never run "${folder}")
    set_tests_properties(proof.${name} PROPERTIES
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                         PASS_REGULAR_EXPRESSION "
${printed}
?
$"
                         SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 120)
    add_test(NAME proof.${name}_watch
             COMMAND "${FBENGINE_FB}" --color never run "${folder}" -- --watch --frames 40)
    set_tests_properties(proof.${name}_watch PROPERTIES
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                         PASS_REGULAR_EXPRESSION "
watched 40 frames
?
$"
                         SKIP_REGULAR_EXPRESSION "no GPU here" TIMEOUT 120)
  endfunction()
  fbengine_add_lane_proof(two_videos_at_startup
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
  fbengine_add_lane_proof(stack_and_joints
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
                        "folder-place;goes above res://")
    list(GET case 0 name)
    list(GET case 1 said)
    add_test(NAME assets.panics.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/assets/panics"
                     -- ${name})
    set_tests_properties(assets.panics.${name} PROPERTIES PASS_REGULAR_EXPRESSION
                         "panic: [^\n]*${said}[^\n]* at [^\n]*assets/panics/main\\.fb:")
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
  set_tests_properties(fbengine.package PROPERTIES TIMEOUT 1800)
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
catch_discover_tests(fbengine_audio_native_tests TEST_PREFIX "audio."
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
  list(JOIN fbengine_skin_proof "?
" fbengine_skin_proof_lines)
  add_test(NAME proof.skinned_animation_with_bones_under_game_control
           COMMAND "${FBENGINE_FB}" --color never run
                   "${PROJECT_SOURCE_DIR}/examples/proof/skinned_animation_with_bones_under_game_control")
  set_tests_properties(proof.skinned_animation_with_bones_under_game_control PROPERTIES TIMEOUT 120
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "
${fbengine_skin_proof_lines}?
$")
  add_test(NAME proof.skinned_animation_with_bones_under_game_control_watch
           COMMAND "${FBENGINE_FB}" --color never run
                   "${PROJECT_SOURCE_DIR}/examples/proof/skinned_animation_with_bones_under_game_control"
                   -- --watch --frames 300)
  set_tests_properties(proof.skinned_animation_with_bones_under_game_control_watch PROPERTIES
                       TIMEOUT 120
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "
watched 300 frames?
$")
  add_test(NAME proof.streamed_and_positional_sound_watch
           COMMAND "${FBENGINE_FB}" --color never run
                   "${PROJECT_SOURCE_DIR}/examples/proof/streamed_and_positional_sound"
                   -- --watch --frames 960)
  set_tests_properties(proof.streamed_and_positional_sound_watch PROPERTIES TIMEOUT 120
                       PASS_REGULAR_EXPRESSION "\nwatched 960 frames\r?\n$")
  # 64 channels at 44100 Hz under a game's load, within the budget.
  add_test(NAME examples.audio_stress
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/audio_stress")
  set_tests_properties(examples.audio_stress PROPERTIES TIMEOUT 120
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
add_executable(fbengine_anim_oom_tests anim/oom_tests.cpp)
target_link_libraries(fbengine_anim_oom_tests PRIVATE fbengine_internal Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_anim_oom_tests)
catch_discover_tests(fbengine_anim_oom_tests TEST_PREFIX "anim." ADD_TAGS_AS_LABELS
                     DL_PATHS "${fbengine_test_dll_paths}"
                     PROPERTIES ENVIRONMENT "${fbengine_vulkan_environment}")

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
