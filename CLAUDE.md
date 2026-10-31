# fbengine

FutureBlitz's game engine, as packages built on the language, never part of it. The plan, its
packages and milestones E0 to E14, is FutureBlitz's docs/engine.md (with assets.md and
migration.md); its decisions go in FutureBlitz's docs/decisions.md.

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
E0, setup (2026-10-31): the hello package; CI (ci.yml) building each target's DLLs (native.yml),
Linux's on glibc 2.28, and running the package's tests and the examples on fb 1.0.0 on all five
targets; and publish.yml, run by hand on a v<version> tag (`gh workflow run publish.yml --ref
v0.1.1 -f registry=staging`), which publishes through trusted publishing and then adds and runs
the package from the registry on every target. fbengine 0.1.0 (from the user's `fb login`) and
0.1.1 (trusted publishing) are on staging, not production. Next: headless Vulkan in CI, then E1.
