# fbengine_video

fbengine's video: MPEG-1 program streams (`.mpg`), decoded by
[pl_mpeg](https://github.com/phoboslab/pl_mpeg) on a thread of their own, a few frames ahead, into
a texture a game draws, with their sound through fbengine_audio, the clock kept to what's been
mixed of it.

```toml
[dependencies]
fbengine = "0.1"
fbengine_audio = "0.1"
fbengine_video = "0.1"
```

```vb
Import std.core
Import fbengine
Import fbengine_audio
Import fbengine_video

Try Graphics(1280, 720)
Try Audio.Start()
Local intro = Try Video.Open("intro.mpg")
intro.Play()
Local canvas = Screen.Canvas()
While !intro.Ended() && !Window.Closed()
    intro.Update(Frame.Delta())
    // As large as it fits, with its shape kept, and black in the bars.
    Try intro.DrawFitted(canvas, Screen.Width(), Screen.Height())
    Flip()
End While
intro.Free()
```

`Texture` has its frame for anything else that draws, as a Mesh's material, and `SetSound` plays a
Sound of the game's beside a video with none. A game makes its videos MPEG-1, with ffmpeg as
`ffmpeg -i in.mp4 -c:v mpeg1video -q:v 4 -c:a mp2 -b:a 192k -f mpeg out.mpg`. Released with
fbengine at the same version, and needs fb 1.2.3 or later. Apache-2.0; in its DLL, pl_mpeg is MIT
(notices/pl_mpeg.txt), and on Windows MinGW-w64's winpthreads is MIT and BSD-style
(notices/winpthreads.txt).
