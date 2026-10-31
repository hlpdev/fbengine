FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.16.1
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_Declare(
  VulkanHeaders
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG v1.4.350
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_Declare(
  volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG 1.4.350
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_MakeAvailable(Catch2 VulkanHeaders volk)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# ctest puts the toolchain's DLLs (MinGW's libstdc++ and friends) on PATH for the tests, so
# they run from any terminal.
set(fbengine_test_dll_paths "")
if(MINGW)
  cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH fbengine_test_dll_paths)
endif()

# Vulkan with no window, on the system's loader, which volk loads: the GPU's own driver on a
# developer's machine, and lavapipe in CI.
add_executable(fbengine_gpu_tests gpu/vulkan_tests.cpp)
target_link_libraries(fbengine_gpu_tests PRIVATE volk Vulkan::Headers Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_gpu_tests)
catch_discover_tests(fbengine_gpu_tests TEST_PREFIX "gpu."
                     DL_PATHS "${fbengine_test_dll_paths}")

# The packages' own tests, run by `fb test`, and the examples, run by `fb run` and checked against
# what they print, each after the DLLs are built into the packages' native/<target>/ folders.
# CI's GPU jobs build without them, and without fb.
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
                           "SDL_VIDEO_DRIVER=offscreen;FBENGINE_TEST_FONTS=${harfbuzz_SOURCE_DIR}/test/api/fonts")
    endif()
  endforeach()

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

  add_test(NAME examples.menu
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/menu")
  set_tests_properties(examples.menu PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen"
                       PASS_REGULAR_EXPRESSION "Running menu[^\n]*\noptions open: True\r?\nvolume 0[.]65, fullscreen True, name Ada\r?\nmenu again: True\r?\nplaying: True, focus none\r?\na jump: score 100\r?\nconsole: [[]> give 50, gave 50[]], score 150, typed \" \"\r?\nclosed, a jump: score 250\r?\n$")
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
add_executable(fbengine_physics_step_tests physics/step_tests.cpp)
target_link_libraries(fbengine_physics_step_tests PRIVATE fbengine_physics_native
                      Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_physics_step_tests)
catch_discover_tests(fbengine_physics_step_tests TEST_PREFIX "physics."
                     DL_PATHS "${fbengine_test_dll_paths};$<TARGET_FILE_DIR:fbengine_physics_native>")

# fbengine_physics' examples, checked against what they print.
if(FBENGINE_FB_TESTS)
  add_test(NAME examples.falling_boxes
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/falling_boxes")
  set_tests_properties(examples.falling_boxes PROPERTIES PASS_REGULAR_EXPRESSION
                       "\n200 boxes fell, 200 came to rest, 100 on others\r?\n$")
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
    set_tests_properties(physics.panics.${name} PROPERTIES PASS_REGULAR_EXPRESSION
                         "panic: PhysicsWorld\\.Step: [^\n]*${said}")
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

# text's atlas and layouts, as the canvas reads them, from its sources and core's, on HarfBuzz's
# test fonts.
file(GLOB fbengine_text_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/native/fbengine/text/*.cpp")
add_executable(fbengine_text_tests text/text_tests.cpp ${fbengine_text_sources}
               "${PROJECT_SOURCE_DIR}/native/fbengine/core/core.cpp")
target_include_directories(fbengine_text_tests PRIVATE "${PROJECT_SOURCE_DIR}/native/fbengine")
target_compile_definitions(fbengine_text_tests PRIVATE FBE_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}"
                           FBE_TEST_FONTS="${harfbuzz_SOURCE_DIR}/test/api/fonts")
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
  # 64 channels at 44100 Hz under a game's load, within the budget.
  add_test(NAME examples.audio_stress
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/audio_stress")
  set_tests_properties(examples.audio_stress PROPERTIES TIMEOUT 120
                       PASS_REGULAR_EXPRESSION "600 frames on no device, up to 64 channels at once\r?\n.*within the budget\r?\n$")
endif()

# fbengine_native's frame, each test in a process of its own, as a program's first frames are.
add_executable(fbengine_core_tests core/frame_tests.cpp)
target_link_libraries(fbengine_core_tests PRIVATE fbengine_native Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_core_tests)
catch_discover_tests(fbengine_core_tests TEST_PREFIX "core."
                     DL_PATHS "${fbengine_test_dll_paths};$<TARGET_FILE_DIR:fbengine_native>")
