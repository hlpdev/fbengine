# Questions for the user

What waits on the user's call, each with a recommendation. Answered ones move to FutureBlitz's
docs/decisions.md and leave this file.

None are open. The four from planning (re-exports, the video decoder, the editor's scope, and
when to publish) were settled on their recommendations, at the user's request, on 2026-10-31.
Publishing to the production registry, at E5, still waits on the user's yes when it comes.

## How a game's machine gets Slang's compiler

E6 step 7 compiles a game's `.slang` files in its build step, which needs `slangc`, of the one release
the engine's are compiled with: from `FBENGINE_SLANGC` or the PATH, or the build stops saying which to
get. A game with no shaders never needs it. It could instead come with fbengine_tools (about 33 MB for
each of 5 targets, and `fb` has no place for a program beside a package's DLLs), or be downloaded by the
build step on first use (a build reaching the network on its own). Left as it is until you say.
