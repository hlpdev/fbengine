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

Sounds are WAV, FLAC, or Ogg Vorbis, told apart by their bytes, decoded whole by `Sound.Load` or
decoded as they play by `Sound.Stream`, for music. A crossfade is one channel's `FadeOut` and the
next one's `Play(fadeIn: ...)`.

`Audio.Start` plays on the OS's default output, which it follows within about a second of its
changing, at a `Flip`, or on one of `Audio.Outputs(AudioDevice.DEFAULT)` by name, as a game's
options let a player pick. When the output's lost, the device stopped or its mix still for a
quarter of a second, the next `Flip` moves the mix to the default one, and `Audio.Lost()` says so
for a frame.

A `Bus` groups channels under one volume, low-pass, and reverb, inside another bus or the whole
mix: music, effects, and voices at the volumes a player sets, or sounds muffled behind a wall.

An `Emitter` plays sounds in the world, heard from the `Listener`: quieter with distance, panned to
their side, and pitched by the Doppler effect. Its channels move with it. An emitter that follows
an entity, or where a function says, and the listener following the camera, are moved at each of
fbengine's Flips, at the velocity of the move over the frame; given the game's `FixedStep`, they're
where the entity is drawn between steps, so they move as smoothly as what's seen. A `Channel` is
one play of a `Sound`; once it ends or is stopped, its methods do nothing, so a game can stop or
ask about one whose sound ended long ago.

It's released with fbengine, at the same version. Apache-2.0; miniaudio is MIT-0 or public
domain.
