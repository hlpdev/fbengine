# fbengine

FutureBlitz's game engine, as packages built on the language, never part of it. docs/plan.md has
the milestones E0 to E14, their steps, and the lanes; docs/architecture.md how it fits together;
docs/questions.md what waits on the user; docs/research/ the read-throughs of Blitz3D's and SCP:CB's
source. FutureBlitz's docs/engine.md is the summary, and its decisions go in FutureBlitz's
docs/decisions.md. fbengine is its own engine, not a modern copy of Blitz3D's; converters for
Blitz code and assets are a game's own tools, never the engine's or the language's.

## How we work
- As in FutureBlitz: you write the code, C++ and .fb, one step at a time, in my code style, and
  verify each step before calling it done: `python tools/verify.py` prints "everything passed"
  (the build, ctest offscreen, the export check, the bindings, clang-format, clang-tidy on what
  changed, `fb fmt --check`).
  Every bug a check or review finds is fixed, with a test, before the step closes.
- Commit each verified step as me (a short lowercase message, no Claude attribution) and push.
- A bug or gap the engine finds in FutureBlitz is fixed there, with a test, never worked around
  here; nothing in FutureBlitz may depend on the engine.
- Tests never open a window: SDL's offscreen driver and a software Vulkan (lavapipe, SwiftShader).
- Keep explanations short.

## Conventions
- C++23, CMake, deps via FetchContent; FutureBlitz's .clang-format and .clang-tidy, copied here.
- snake_case; types end in _t; private/protected members use m_; enum constants UPPER_CASE;
  template params CamelCase; never align declarations or assignments; spaces inside braced lists.
- Each package's DLL (cmake/native.cmake's fbengine_add_native) is C++ behind a C API of
  fixed-size types, exporting only its `fbe_` functions, with its C++
  runtime linked in. Every package's functions are `fbe_<area>_<verb>` (`fbe_window_open`,
  `fbe_audio_play`, `fbe_body_add`), so one export check covers them all; handles are
  `uint64_t`.
- The C API is split by area: native/<package>/<area>/fbe_<area>.h, its .cpp files beside it
  (CMake takes every area folder's), and its bindings, packages/<package>/c/fbe_<area>.fb, which
  `sh tools/bindgen.sh` makes again whenever a header changes. An area's header uses only
  fixed-size types and its own structs, never another area's.
- A package's FB API is objects with properties and methods, `@MainThread`, over its bindings.
- The packages' versions are one: packages/fbengine/project.toml's, which CMake reads; every
  package's project.toml and the tests that name it change with it.

## Layout
- native/<package>/<area>/: an area's C API header and its C++; native/fbengine/fbengine.h
  includes every area's.
- packages/<package>/: project.toml, the FB code, c/ (bindings), tests/ (`fb test`).
- tools/: verify.py, bindgen.sh, check-exports.sh.
- examples/: programs on the packages, which ctest runs.

## Current step
E0 is done, and E1 steps 1 and 2 (2026-10-31; FutureBlitz's docs/history.md has what each added).
The planning pass (docs/plan.md, architecture.md, questions.md, research/, lanes.md) and the lane
prep are done. Six lanes run from 2026-10-31, as docs/plan.md's Lanes table says: input (E1, E11),
gpu (E2, E3, E6), scene (E4, E5, E7), assets (jobs, E8), audio (E9, E12), physics (E10, E13),
each on its branch (lane-<name>) in its own worktree. The coordinator reviews and lands each step.

## CI notes
- Windows' GPU job uses mesa-dist-win's MSVC lavapipe, registered in HKLM, since the loader
  ignores VK_DRIVER_FILES for an administrator and MSYS2's MinGW lavapipe corrupts its heap.
- Intel Macs have no GPU job: no Metal on their runners, no lavapipe in Homebrew's Mesa.
- Publishing: `gh workflow run publish.yml --ref v<version> -f registry=staging` on a tag of the
  version in every package's project.toml.
