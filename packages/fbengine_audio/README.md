# fbengine_audio

fbengine's sound, on miniaudio: the mix and its device, then sounds, channels, music streams, and
3D sound that follows fbengine's entities.

```toml
[dependencies]
fbengine_audio = "0.1"
```

```vb
Import std.core
Import std.io
Import fbengine_audio

Try Audio.Start()
Local drip = Try Sound.Load(Path("sfx/drip.ogg"))
Local channel = drip.Play()
channel.SetPitch(1.2)
```

Sounds are WAV, FLAC, or Ogg Vorbis, told apart by their bytes. A `Channel` is one play of a
`Sound`; once it ends or is stopped, its methods do nothing, so a game can stop or ask about one
whose sound ended long ago.

It's released with fbengine, at the same version. Apache-2.0; miniaudio is MIT-0 or public
domain.
