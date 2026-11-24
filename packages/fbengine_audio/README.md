# fbengine_audio

fbengine's sound, on miniaudio: the mix and its device, then sounds, channels, music streams, 3D
sound that follows fbengine's entities, and voice chat's pieces: the microphone, a sound fed as it
comes, Opus, and noise suppression.

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
fbengine's Flips, at the velocity of the move over the frame; given the game's `FixedUpdate`,
they're
where the entity is drawn between steps, so they move as smoothly as what's seen. A `Channel` is
one play of a `Sound`; once it ends or is stopped, its methods do nothing, so a game can stop or
ask about one whose sound ended long ago.

For voice chat, the `Microphone` hears an input, apart from the mix, and `Read` gives what it's
heard each frame. A `Denoiser` (RNNoise) takes the noise out of each 10 ms of it and says how
likely it's a voice; a `VoiceEncoder` (Opus) compresses 20 ms at a time to a packet of about 60
bytes, and a `VoiceDecoder` makes speech of it again, making up a packet that's lost. What's
decoded is pushed to a `Sound.Feed`, which plays it on one channel, from an emitter where its
speaker stands, buffering a little so packets that come unevenly don't stutter.

It's released with fbengine, at the same version, and needs fb 1.3.0 or later. Apache-2.0; in its
DLL, miniaudio is MIT-0 or public domain (notices/miniaudio.txt), with stb_vorbis, MIT or public
domain (notices/stb_vorbis.txt), and verblib, MIT-0 or public domain (notices/verblib.txt); Opus is
BSD-3-Clause, with royalty-free patent licenses (notices/opus.txt), and RNNoise BSD-3-Clause
(notices/rnnoise.txt); and on Windows MinGW-w64's winpthreads is MIT and BSD-style
(notices/winpthreads.txt).
