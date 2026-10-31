# fbengine

FutureBlitz's game engine, as packages built on the language, never part of it. docs/plan.md has
the milestones E0 to E14, their steps, and the lanes; docs/architecture.md how it fits together;
docs/questions.md what waits on the user; docs/research/ the read-throughs of Blitz3D's and SCP:CB's
source. FutureBlitz's docs/engine.md is the summary, and its decisions go in FutureBlitz's
docs/decisions.md. fbengine is a modern engine and copies none of Blitz3D's quirks.

## How we work
- As in FutureBlitz: you write the code, C++ and .fb, one step at a time, in my code style, and
  verify each step before calling it done: ctest, clang-format, clang-tidy, and `fb fmt --check`.
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
  fixed-size types, exporting only its prefix's functions (`fbe_` for fbengine), with its C++
  runtime linked in. Its FB bindings are fb-bindgen's, in the package's c/ folder, made again
  whenever the header changes.
- A package's FB API is objects with properties and methods, `@MainThread`, over its bindings.
- The packages' versions are one: packages/fbengine/project.toml's, which CMake reads; every
  package's project.toml and the tests that name it change with it.

## Layout
- native/<package>/: each DLL's C API header and its C++.
- packages/<package>/: project.toml, the FB code, c/ (bindings), tests/ (`fb test`).
- examples/: programs on the packages, which ctest runs.

## Current step
E0 is done, and E1 steps 1 and 2 (2026-10-31; FutureBlitz's docs/history.md has what each added).
The engine's planning pass (2026-10-31) wrote docs/plan.md, architecture.md, questions.md, and
research/; next, the user's answers to questions.md's first five, then the step that splits the
headers, bindings, and folders by area and adds tools/verify.py, then the first wave of lanes.

## CI notes
- Windows' GPU job uses mesa-dist-win's MSVC lavapipe, registered in HKLM, since the loader
  ignores VK_DRIVER_FILES for an administrator and MSYS2's MinGW lavapipe corrupts its heap.
- Intel Macs have no GPU job: no Metal on their runners, no lavapipe in Homebrew's Mesa.
- Publishing: `gh workflow run publish.yml --ref v<version> -f registry=staging` on a tag of the
  version in every package's project.toml.
