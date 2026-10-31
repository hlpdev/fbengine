# How a lane works

A lane is a Claude Code session building one milestone's steps, in a worktree of this repository
on a branch of its own. The coordinator, the session that started it, reviews each step and lands
it on master. CLAUDE.md, docs/plan.md, and docs/architecture.md hold for every lane.

## Setting up

The worktree is the lane's own; build in a folder beside it, never in another lane's:

```bash
export PATH="/c/Users/Hunter/AppData/Local/Programs/CLion/bin/mingw/bin:/c/Users/Hunter/AppData/Local/Programs/CLion/bin/ninja/win/x64:/c/Program Files/CMake/bin:/c/Users/Hunter/.futureblitz/bin:$PATH"
cmake -S . -B ../build-<lane> -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build ../build-<lane>
```

fb is the installed release (`fb --version`); fb-bindgen is beside it.

## Each step

1. **Design** the step's API first, from docs/plan.md and docs/architecture.md, as fb doc comments
   and the C header, and check it against docs/research/ for what SCP:CB needs of it. A call the
   docs settle, the lane makes and says so in its report; a real trade-off with no clear answer
   goes to the coordinator before code.
2. **Build** it: the C++ in `native/<package>/<area>/`, the header `fbe_<area>.h`, then
   `sh tools/bindgen.sh`, then the FB API in `packages/<package>/`, `@MainThread` Functions and
   Methods, Final Classes, checked Errors for what can fail at run time, panics for mistakes.
3. **Test** it: C++ unit tests (Catch2, in `tests/`) for what's hot or subtle, FB tests in the
   package's `tests/` for the API, and an example in `examples/` when the step is something a game
   does. Tests never open a window: `Window.SetHeadless(True)` first, SDL's offscreen driver, and
   lavapipe for GPU tests. A test that waits on time leaves room for a slow CI machine.
4. **Check** it: `python tools/verify.py --build ../build-<lane>` prints "everything passed".
   Nothing it finds is left, whatever its size.
5. **Commit and push**: as hlpdev <hunterpollock2006@gmail.com>, short lowercase messages, small
   commits, no Claude attribution, no "wip" left. Push the branch. Never force-push: a rebased
   branch whose old commits were pushed goes up as `<branch>-2`.
6. **Report** to the coordinator: the branch, the last commit, verify.py's result, the decisions
   taken, and a paragraph for the history, in docs/history.md's style (what the step added, by
   name). Then start the next step, unless the coordinator says otherwise.

## Rules

- **Files**: a lane edits its own area (docs/plan.md's Lanes table) and adds what its step needs.
  Shared files (docs/plan.md's list) take additions only, at the end of a list, so landings merge
  cleanly. Another lane's files go through the coordinator.
- **Waiting**: a step that needs another lane's unlanded work builds what it can without it, says
  what's left in its report, and finishes once that lands on master (rebase onto it then).
- **The coordinator alone** edits docs/plan.md, docs/architecture.md, docs/questions.md, CLAUDE.md,
  and FutureBlitz's docs.
- **Libraries**: the ones docs/plan.md and architecture.md name, through FetchContent, pinned to a
  release tag, statically linked, with their licenses noted in the step's report. Any other goes
  to the coordinator first.
- **FutureBlitz**: a bug or gap the lane finds in the language, fb, or std is reported to the
  coordinator with a small program that shows it, never worked around in the engine.
- **CI**: a lane never watches or polls CI; verify.py is its check. The coordinator watches
  landings.
- **No windows** on this machine, ever: no test, example, or check may open one.
- **Processes**: stop only what the lane started, by its exact path; never by a pattern.

## Review and landing

The coordinator reviews each reported step: reviewers over its parts, each finding reproduced
before it counts. The lane fixes every confirmed finding with a test, runs verify.py, pushes, and
reports again. The coordinator rebases the branch onto master (keeping its commits), runs
verify.py on the result, fast-forwards master only once it prints "everything passed", watches
CI, and updates docs/plan.md and the history.
