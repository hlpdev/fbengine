# fbengine_physics

fbengine's physics, on [Jolt](https://github.com/jrouwe/JoltPhysics): worlds of rigid bodies,
stepped on Jolt's own threads, in std.math's axes and units (right-handed, Y up, meters, seconds).

```toml
[dependencies]
fbengine = "0.1"
fbengine_physics = "0.1"
```

```vb
Import std.core
Import std.math
Import fbengine_physics

Local world = PhysicsWorld()
// A floor that never moves, and a crate that falls onto it.
Body(world, Shape.Box(Vec3(20, 1, 20)), Motion.STATIC, Vec3(0, -0.5, 0))
Local crate = Body(world, Shape.Box(Vec3(1, 1, 1)), Motion.DYNAMIC, Vec3(0, 5, 0))
For frame = 1 To 120
    // Steps at 60 a second for the time a frame took, however long that was.
    world.Update(1.0 / 60)
End For
Print("the crate is at {crate.Position()}")
world.Free()
```

Bodies are static, dynamic, or kinematic, of boxes, spheres, capsules, cylinders, convex hulls,
meshes, and compounds of them, in layers, held by joints, noticed by sensors, and found by casts;
kinematic ones are driven where a game puts them, fast ones collide all along their way, and
characters walk among them. A body keeps an entity of fbengine's where it is with `Drive`, or goes
where an entity is with `Follow`, a batch a frame. A world is deterministic: made and stepped by the same calls in the
same order, it moves the same, bit for bit, on any number of threads, whatever floating point mode
the game's thread is in, `Save` and `Restore` take it back to where it was, and `StateHash` says,
for a game in lockstep, whether two have gone apart. Jolt is built to
make it the same on every target, compiler, and build too. Its tests, which run each world twice
and compare, pass on each of the five targets; a hash from one target isn't yet compared with
another's.

A saved state is for rewinding and replays: it restores only in the build that saved it, the same
fbengine_physics and Jolt, and `Restore` refuses one from any other, a patch release's included.
It isn't a save file, so a game keeps its own save of what a player's done.

Released with fbengine at the same version, and needs fb 1.3.0 or later. Apache-2.0; in its DLL,
Jolt is MIT (notices/jolt.txt), and on Windows MinGW-w64's winpthreads is MIT and BSD-style
(notices/winpthreads.txt).
