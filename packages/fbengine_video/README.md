# fbengine_video

fbengine's video: MPEG-1 program streams (`.mpg`), decoded by
[pl_mpeg](https://github.com/phoboslab/pl_mpeg) on a thread of their own, a few frames ahead,
into frames in memory, with their sound.

```toml
[dependencies]
fbengine_video = "0.1"
```

```vb
Import std.core
Import fbengine_video

Local intro = Try Video.Open("intro.mpg")
intro.Play()
While !intro.Ended()
    // A frame's time, as a game's loop gives it.
    intro.Update(1.0 / 60)
    Local frame = intro.Frame()
End While
intro.Free()
```

A game makes its videos MPEG-1, with ffmpeg as
`ffmpeg -i in.mp4 -c:v mpeg1video -q:v 4 -c:a mp2 -b:a 192k -f mpeg out.mpg`. Released with
fbengine at the same version. Apache-2.0; pl_mpeg, in its DLL, is MIT (notices/pl_mpeg.txt).
