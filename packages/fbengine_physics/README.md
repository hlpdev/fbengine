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
world.SetGravity(Vec3(0, -9.81, 0))
// Steps at 60 a second for the time a frame took, however long that was.
world.Update(1.0 / 30)
Print("{world.Steps()} steps")
world.Free()
```

Released with fbengine at the same version. Apache-2.0; Jolt, in its DLL, is MIT
(notices/jolt.txt).
