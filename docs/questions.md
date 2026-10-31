# Questions for the user

What waits on the user's call, each with a recommendation. Answered ones move to FutureBlitz's
docs/decisions.md and leave this file.

## Before E8

1. **Re-exports** (`Public Import`): only needed for an umbrella `fbengine_all`. Recommended:
   leave it.

## Before E12

2. **Video decoder**: recommended pl_mpeg (MPEG-1, one header, MIT), with a game shipping its
   videos as MPEG-1; or dav1d (AV1) for quality, at many times the size.

## Before E13

3. **The editor's scope**: recommended a scene and asset editor (scene view, tree, inspector,
   asset browser, play in the editor) built on fbengine_ui, with code staying in VS Code and
    fb-lsp; FutureBlitz's own IDE is a separate, later project. The inspector needs FutureBlitz to
    set reflected Properties (its Deferred work).

## When to publish

4. **The packages on the production registry**: recommended at E5, as 0.x, once a game can draw
    a scene; staging until then.
