# fbengine_audio

fbengine's sound, on miniaudio: the mix and its device, then sounds, channels, music streams, and
3D sound that follows fbengine's entities.

```toml
[dependencies]
fbengine_audio = "0.1"
```

```vb
Import std.core
Import fbengine_audio

Try Audio.Start()
Print("{Audio.SampleRate()} Hz on {Audio.DeviceName()}")
Audio.Stop()
```

It's released with fbengine, at the same version. Apache-2.0; miniaudio is MIT-0 or public
domain.
