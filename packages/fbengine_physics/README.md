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
meshes, and compounds of them.

Released with fbengine at the same version. Apache-2.0; Jolt, in its DLL, is MIT
(notices/jolt.txt).
