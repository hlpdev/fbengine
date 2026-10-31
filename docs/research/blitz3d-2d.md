# Blitz3D to FutureBlitz: the 2D group (bbgraphics)

Every command in `b3d-commands/bbgraphics.txt` (111), from `bbruntime/bbgraphics.cpp` and the DX7
layer under it (`gxruntime/gxcanvas.cpp`, `gxgraphics.cpp`, `gxfont.cpp`, `gxruntime.cpp`,
`ddutil.cpp`, `gxmovie.cpp`), checked against the help pages in `_release/help/commands/`.

Names the tables use (proposed, fbengine.gfx2d unless said):

- `DrawBuffer`: anything 2D draws into, Blitz's "buffer": the screen's, each image frame's, and
  later a texture's (E4/E5's TextureBuffer gives the same type).
- `Screen`: a Class of Static Methods for the screen's buffer and its size (`Screen.Buffer()`,
  `Screen.Width()`), and Blitz's graphics-mode text console (`Screen.Print`).
- The current-buffer "pen" stays Blitz-shaped Functions (`SetBuffer`, `SetDrawColor`, `Cls`,
  `DrawRect`), because it's an app-level singleton whose state rules (what SetBuffer resets) have
  to match Blitz's exactly; see Design question 1.
- `Image` (frames, a handle, a mask), `Font`, `FontStyle`, `Gamma`, and `Movie` (a new package).
- Colors are std.math's `Color32`; coordinates are `Int32`, as Blitz's are (Design question 3).

All of it is `@MainThread`. Resources (`Image`, `Font`, `Movie`) have `Free()` and generational
handles. Loads throw `IOError` where Blitz returned 0; the migrator emits a `Try` form where the
game compares the handle with 0.

## Graphics drivers and modes

E1 has `Displays()`: each `Display` has `Name`, `Desktop`, and `Modes` (largest first), each
`DisplayMode` `Width`, `Height`, `RefreshRate`, `PixelDensity`. Blitz's "drivers" were
DirectDraw devices, one per monitor in practice (`gxruntime.cpp` `enumDriver`), so a driver is a
Display. Blitz's indexes are 1-based; Displays() is 0-based.

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `CountGfxDrivers()` | number of graphics drivers (monitors) | `Displays().Count` | fbengine E1 | The first call re-enumerates with secondary and non-display devices (`numGraphicsDrivers` sets `enum_all`). |
| `GfxDriverName$(driver)` | a driver's description | `Displays()[driver - 1].Name` | fbengine E1 | 1-based; out of range is a debug-mode runtime error ("Illegal graphics driver index"). Name can be "" in E1. |
| `SetGfxDriver driver` | picks the driver the next Graphics uses | `Window.SetDisplay(Displays()[driver - 1])` | fbengine E1 (left) | Not in E1's steps: the display a fullscreen window goes on. In Blitz a windowed Graphics always uses driver 0 (`openGraphics`: `if( windowed ) driver=0`). Clears the cached mode list. |
| `CountGfxModes()` | number of fullscreen modes of the current driver | `Displays()[d].Modes.Count` | fbengine E1 | Blitz lists each size once per depth (16, 24, 32) and counts duplicates; SDL lists each size once per refresh rate. Menus that list modes should dedupe by size: the migrator emits a `Distinct` over `(Width, Height)`. Order differs (Blitz: DirectDraw's; E1: largest first), so a hard-coded mode index won't match. |
| `GfxModeExists(width, height, depth)` | whether a fullscreen mode exists | `Displays()[d].Modes.Any(m => m.Width == width && m.Height == height)` | fbengine E1 | depth is dropped (always 32-bit). Blitz compares exact w, h, d. |
| `GfxModeWidth(mode)` | a mode's width | `Displays()[d].Modes[mode - 1].Width` | fbengine E1 | 1-based, over the list the last CountGfxModes made. |
| `GfxModeHeight(mode)` | a mode's height | `Displays()[d].Modes[mode - 1].Height` | fbengine E1 | As above. |
| `GfxModeDepth(mode)` | a mode's color depth | `32` | dropped | Every mode is 32-bit RGBA8 now; the migrator emits the constant 32. |
| `GfxDriver3D(driver)` | whether a driver can do 3D | `True` | dropped | Every Vulkan device does 3D; the migrator emits True. |
| `CountGfxModes3D()` | number of modes that can do 3D | `Displays()[d].Modes.Count` | fbengine E1 | Same list as CountGfxModes. |
| `GfxMode3DExists(width, height, depth)` | whether a 3D-capable mode exists | as GfxModeExists | fbengine E1 | Same. |
| `GfxMode3D(mode)` | whether a mode can do 3D | `True` | dropped | Migrator emits True. |
| `Windowed3D()` | whether 3D works in a window | `True` | dropped | Migrator emits True. |
| `TotalVidMem()` | video memory in bytes | `Gpu.MemoryBudget()` | fbengine E2 | DX7's `dwVidMemTotal`, an Int that saturates past 2 GB; E2 gives VMA's heap budget (VK_EXT_memory_budget) as Int64. Games only print it or compare it with a threshold. |
| `AvailVidMem()` | free video memory | `Gpu.MemoryBudget() - Gpu.MemoryUsage()` | fbengine E2 | DX7's `dwVidMemFree`. |

## Display mode

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `Graphics width, height[, depth][, mode]` | opens the 2D display | `Graphics(width, height, WindowMode.X)` | fbengine E1 | depth dropped. mode (`bbGraphics`): 0 = windowed in debug, fullscreen in release; 1 = FULLSCREEN; 2 = WINDOWED; 3 = WINDOWED, scaled (the buffer stays w x h and is stretched to a resizable window: `Window.SetResizable(True)` plus E3's `Screen.SetScaling(...)`); 6 and 7 = 2 and 3 with auto-suspend (`Frame.SetPauseInBackground(True)`, left for E1's step 7); any other value is a runtime error "Illegal Graphics mode". Fullscreen always auto-suspends in Blitz. It resets the pen: Color to white, ClsColor to black, the font to the default, and SetBuffer to the FrontBuffer (2D) or BackBuffer (3D), so origin and viewport too. It frees every image, font, movie, and the 3D world (`freeGraphics`, `~gxGraphics`); fbengine keeps them (a valid game never used the old handles). Mode 0: Design question 6. |
| `Graphics3D width, height[, depth][, mode]` | opens the 3D display | `Graphics(width, height, WindowMode.X)` | fbengine E1 | Same as Graphics, but mode 0 is windowed only in debug and only if Windowed3D; the current buffer is the BackBuffer. In fbengine there's no 2D-only mode: the scene (E4) is there whenever the window is. |
| `EndGraphics` | leaves graphics mode | `EndGraphics()` | fbengine E1 | Blitz frees images etc. and reopens its hidden 400x300 window with the pen reset; E1 closes the window. |
| `GraphicsLost()` | whether a fullscreen display was lost (alt-tab) | `False` | dropped | DX7's lost surfaces (`gfx_lost` when `restore` fails with DDERR_WRONGMODE). Vulkan swapchains are recreated by the engine, never lost to the game; the migrator emits False. |

What E1 leaves for these: `Window.SetDisplay` (SetGfxDriver); a screen buffer of the size Graphics
asks for in pixels, scaled to the window (mode 3, and high DPI: E1's Graphics takes desktop units,
Blitz's takes pixels; Design question 2); pause in background (modes 6, 7, and fullscreen); the
OS cursor hidden in fullscreen (`setPointerVisible` returns early in exclusive mode, so Blitz's
fullscreen never shows it); the pen's reset in Graphics; and Flip presenting a copy, so the buffer
keeps its pixels across Flip (E2's present; see Buffers).

## Gamma

Blitz keeps a 256-entry 16-bit ramp per channel (`gxGraphics::_gammaRamp`), identity at start,
and DX7 applies it only in fullscreen (windowed: `_gamma` is 0, UpdateGamma does nothing).
SDL3 has no gamma ramps, so fbengine applies it as a lookup in its present pass, windowed too.

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `SetGamma src_red, src_green, src_blue, dest_red#, dest_green#, dest_blue#` | sets one entry of each channel's ramp | `Gamma.Set(srcRed, srcGreen, srcBlue, Vec3(destRed, destGreen, destBlue))` | fbengine E3 | Three independent entries (index `src & 255` in each channel). dest clamped to 0..255, stored as `dest * 257` in a WORD. Takes effect at UpdateGamma. |
| `UpdateGamma [calibrate]` | applies the ramp | `Gamma.Apply()` | fbengine E3 | calibrate (DDSGR_CALIBRATE) dropped. fbengine uploads the 3 x 256 LUT for the next present. |
| `GammaRed#(red)` | the red ramp's entry | `Gamma.Red(index)` | fbengine E3 | Returns `WORD(dest * 257) / 257`, so a value set comes back truncated to 1/257ths. Index `& 255`. |
| `GammaGreen#(green)` | the green ramp's entry | `Gamma.Green(index)` | fbengine E3 | As above. |
| `GammaBlue#(blue)` | the blue ramp's entry | `Gamma.Blue(index)` | fbengine E3 | As above. |

## Buffers and the frame

In Blitz's windowed modes the FrontBuffer and the BackBuffer are the same surface
(`openWindowedGraphics` passes `fs,fs`), Flip copies it to the window (`paint`), and a 100 ms
timer repaints it whenever it changed (`timerCallback`); in fullscreen they're a DirectDraw flip
chain. fbengine has one screen buffer, kept across Flip, which is Blitz's windowed behavior.

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `FrontBuffer()` | the displayed buffer | `Screen.Buffer()` | fbengine E3 | Same buffer as BackBuffer, as in Blitz's windowed modes. A program that draws to it and never Flips must get Flips from the migrator (Design question 4). |
| `BackBuffer()` | the buffer drawn off screen | `Screen.Buffer()` | fbengine E3 | Contents persist across Flip (windowed Blitz); fullscreen Blitz's held the frame before last, which no game should rely on. |
| `ScanLine()` | the display's current scan line | `0` | dropped | No modern API reports it; the migrator emits 0 (it's only ever printed or busy-waited on; a busy-wait becomes `Flip(True)`). |
| `VWait [frames]` | waits for the vertical blank | `Frame.WaitVBlank()` | fbengine E1 (left) | Blitz ignores `frames` (`bbVWait` waits once). `VWait : Flip False` (the help's idiom) becomes `Flip(True)`. A lone VWait sleeps until the next estimated vblank from the display's RefreshRate. |
| `Flip [vwait]` | shows the back buffer | `Flip(vwait)` | fbengine E1 | Done in E1 (vsync, frame limit, events, `Scheduler.Main.Step()`); E2 presents. Windowed Blitz: `vwait` waits for vblank, then paints only if the buffer changed. |
| `GraphicsWidth()` | the display buffer's width | `Screen.Width()` | fbengine E3 | The screen buffer's pixels, which Graphics' width sets, not the window's units or pixels (Design question 2). |
| `GraphicsHeight()` | the display buffer's height | `Screen.Height()` | fbengine E3 | As above. |
| `GraphicsDepth()` | the display's color depth | `32` | dropped | Always 32. 16-bit quirks (masks matching near-black, ColorRed quantized) are gone; see Behaviors. |
| `SetBuffer buffer` | sets the buffer drawing goes to | `SetBuffer(buffer)` | fbengine E3 | Resets that buffer's origin to 0,0 and viewport to the whole buffer, and the Print cursor to 0,0, and reapplies the global Color, ClsColor, and font (`bbSetBuffer`), even when it's already the current one. |
| `GraphicsBuffer()` | the current buffer | `CurrentBuffer()` | fbengine E3 | |
| `LoadBuffer(buffer, file$)` | loads a picture into a buffer, scaled to fit | `buffer.Load(path) Throws IOError` | fbengine E3 | Scales the file to the buffer's size through TFormImage's resampler (so TFormFilter applies), then draws it solid at 0,0, clipped by the viewport. Sets the buffer's origin to 0,0 and never restores it (`bbLoadBuffer`). Returns 0 on failure: the migrator emits a Try. |
| `SaveBuffer(buffer, file$)` | saves a buffer as a picture | `buffer.Save(path, ImageFormat.BMP) Throws IOError` | fbengine E3 | Always a bottom-up 24-bit BMP whatever the extension (`saveCanvas`), the whole buffer, ignoring the viewport. Built on `buffer.ToImage()` (an std.image `Image`) and std's `Bmp.Encode` and `File.WriteBytes`. |
| `BufferDirty buffer` | backs a buffer up so it survives a lost display | (nothing) | dropped | DX7's surface backup (`gxCanvas::backup`); nothing's lost now. The migrator drops the call. |

## Pixel access

On a modern GPU a buffer lives in GPU memory, so each DrawBuffer gets a CPU staging copy:
`Lock()` makes the CPU copy current (a readback, with a wait, only when the GPU side is newer),
pixel reads and writes go to it, and the next GPU use uploads the rows that changed. Coherency is
lazy and tracked per buffer: a run of ReadPixel or WritePixel calls with no drawing between them
costs one readback; drawing between pixel writes costs a round trip each time, which a debug
counter reports.

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `LockBuffer [buffer]` | locks a buffer for the Fast pixel commands | `buffer.Lock()` | fbengine E3 | Default is the current buffer (`buff ? buff : gx_canvas`). Nests (`locked_cnt`). In Blitz only the pixel commands may be used while locked; fbengine panics on drawing into a locked buffer. |
| `UnlockBuffer [buffer]` | unlocks it | `buffer.Unlock()` | fbengine E3 | Marks the changed rows for upload; the collision mask is rebuilt if the pixels changed (`unlock`). |
| `ReadPixel(x, y[, buffer])` | reads a pixel's ARGB | `buffer.ReadPixel(x, y): Color32` | fbengine E3 | Adds the buffer's origin; outside the viewport it returns the buffer's mask color (`getPixel`), black for the screen. Alpha is always 255 (`argbfill`, the surfaces have no alpha), so Blitz's Int is `$FFrrggbb`, negative. The migrator emits `.ToArgb()` (new in std: `Color32.ToArgb(): UInt32` and `Color32.FromArgb(argb: UInt32)`) where the game treats it as an Int. Auto-locks. |
| `WritePixel x, y, argb[, buffer]` | writes a pixel | `buffer.WritePixel(x, y, color)` | fbengine E3 | Adds the origin and ignores pixels outside the viewport; alpha is dropped. Auto-locks. |
| `ReadPixelFast(x, y[, buffer])` | reads a pixel of a locked buffer | `buffer.ReadPixelFast(x, y): Color32` | fbengine E3 | Absolute coordinates: no origin, no viewport (`getPixelFast`). Blitz reads past the surface on a bad x, y; fbengine panics, and panics when the buffer isn't locked. |
| `WritePixelFast x, y, argb[, buffer]` | writes a pixel of a locked buffer | `buffer.WritePixelFast(x, y, color)` | fbengine E3 | As ReadPixelFast; Blitz corrupts memory out of range, fbengine panics. |
| `CopyPixel src_x, src_y, src_buffer, dest_x, dest_y[, dest_buffer]` | copies a pixel between buffers | `CopyPixel(source, srcX, srcY, destX, destY, dest)` | fbengine E3 | Each side adds its own origin and clips by its own viewport (`copyPixel`). src_buffer has no default but 0 means the current buffer. A raw copy of the pixel's bits, mask and all. |
| `CopyPixelFast src_x, src_y, src_buffer, dest_x, dest_y[, dest_buffer]` | the same on locked buffers | `CopyPixelFast(source, srcX, srcY, destX, destY, dest)` | fbengine E3 | Absolute coordinates on both; both locked. |

## The pen: origin, viewport, colors, Cls

Color, ClsColor, and the font are global (`curr_color`, `curr_clsColor`, `curr_font`, put on
each buffer by SetBuffer); origin and viewport are per buffer and reset by SetBuffer.

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `Origin x, y` | offsets later drawing | `SetOrigin(x, y)` | fbengine E3 | Added to every drawing command's position, and to ReadPixel, WritePixel, CopyPixel; not to the viewport, Cls, CopyRect's source, the Fast pixel commands, the image collisions, or DrawMovie. GrabImage adds it. |
| `Viewport x, y, width, height` | clips later drawing | `SetViewport(x, y, width, height)` | fbengine E3 | In absolute buffer coordinates, origin not applied; clipped to the buffer, and empty when it misses it (`setViewport`). |
| `Color red, green, blue` | sets the drawing color | `SetDrawColor(Color32(red, green, blue))` | fbengine E3 | Blitz packs `(r Shl 16) Or (g Shl 8) Or b` without masking, so 256 or -1 spill into the next channel (Color 0,256,0 is red 1). The migrator emits `Color32.FromArgb(...)` of that packing when the arguments aren't constants in 0..255. Alpha forced to 255. Named SetDrawColor because `Color` is std.math's type. |
| `GetColor x, y` | sets the drawing color from a pixel | `SetDrawColor(CurrentBuffer().ReadPixel(x, y))` | fbengine E3 | ReadPixel's rules: origin added, mask color outside the viewport. |
| `ColorRed()` | the drawing color's red | `DrawColor().r` | fbengine E3 | Read back from the buffer's pixel format, so exact at 32-bit. |
| `ColorGreen()` | its green | `DrawColor().g` | fbengine E3 | |
| `ColorBlue()` | its blue | `DrawColor().b` | fbengine E3 | |
| `ClsColor red, green, blue` | sets the color Cls clears with | `SetClearColor(Color32(red, green, blue))` | fbengine E3 | Same packing as Color. |
| `SetFont font` | sets the font Text uses | `SetFont(font)` | fbengine E3 | Also the font Print, Write, Input, FontWidth, and StringWidth use. |
| `Cls` | clears the buffer | `Cls()` | fbengine E3 | Fills only the current viewport, origin ignored (`gxCanvas::cls`), with ClsColor. |

## Primitives and text

Blitz's primitives are integer spans (DirectDraw color fills and Bresenham pixels). To match
golden images, fbengine generates the same pixels on the CPU (Bresenham's points, the oval's
spans) and draws them as one batch of 1-pixel-high rects on the GPU.

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `Plot x, y` | draws a pixel | `Plot(x, y)` | fbengine E3 | Origin, viewport. |
| `Line x1, y1, x2, y2` | draws a line | `DrawLine(x1, y1, x2, y2)` | fbengine E3 | Bresenham, both ends drawn; Cohen-Sutherland clipped to the viewport with C's truncating integer division first, which can move the pixels of a clipped line (`gxCanvas::line`). A zero-length line goes through setPixel, which adds the origin a second time. |
| `Rect x, y, width, height[, solid]` | draws a rectangle | `DrawRect(x, y, width, height, solid)` | fbengine E3 | solid defaults to True. Outline: four 1-pixel strips, each clipped. width or height <= 0 draws nothing. |
| `Oval x, y, width, height[, solid]` | draws an ellipse in a box | `DrawOval(x, y, width, height, solid)` | fbengine E3 | solid defaults to True. Port `gxCanvas::oval` exactly: center `(x + w/2 + .5, y + h/2 - .5)`, each row's half-width `sqrt(r² - dy²) * w/h`, spans `[floor(cx - hw), floor(cx + hw))`; the outline joins each row's span to the last. It isn't symmetric. |
| `Text x, y, string$[, center_x][, center_y]` | draws text | `DrawText(text, x, y, centerX, centerY)` | fbengine E3 | (x, y) is the top left of the text's cell, not a baseline. Centering subtracts `StringWidth / 2` and `FontHeight / 2` (integer). Each character advances by its cell (see Fonts), no kerning. Characters wholly left of the viewport are skipped, the rest clipped. The drawing color, except black, which draws as RGB(0, 0, 16) (`gxFont::render`). Blitz's strings are bytes in Windows-1252; FB's are UTF-8, so the migrator converts literals and the font maps code points. |
| `CopyRect src_x, src_y, width, height, dest_x, dest_y[, src_buffer][, dest_buffer]` | copies a rectangle between buffers | `CopyRect(srcX, srcY, width, height, destX, destY, source, dest)` | fbengine E3 | Both default to the current buffer. A solid copy (mask ignored). The destination adds its origin and clips by its viewport; the source rect is in absolute coordinates, clipped to the whole source. If the source is an image's buffer, its handle is subtracted from the destination (it goes through `blit`). Overlapping copies within one buffer are DirectDraw's Blt, which handles overlap; fbengine copies through a temporary. |

## Fonts

Blitz's fonts are GDI's (`gxGraphics::loadFont`): `CreateFont(height, ...)` with the system's
font smoothing turned off, every character of the font's range drawn white on black into one
surface, and a character's cell `GetTextExtentPoint32`'s width plus any negative A and C widths
of `GetCharABCWidths`, the glyph drawn `-A` in. The default font is "courier" at 12, which on
Windows is the raster Courier (courier.fon).

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `LoadFont(fontname$[, height][, bold][, italic][, underline])` | loads a font | `Font.System(name, height, FontStyle(bold, italic, underline))` or `Font.Load(path, height, style) Throws IOError` | fbengine E3 | height is the cell height in pixels (`tmHeight`, internal leading included), not points as the help says; default 12. A name with a '.' is a file: Blitz installs it, then asks GDI for the face named after the file's base name (`filenamefile(f.substr(0,n))`), so it works only when they match; fbengine loads the file itself. A system name always gives a font in Blitz, as GDI substitutes; fbengine needs a lookup by family on each OS and a fallback (Design question 7). Converting the cell height to std.image `Font`'s pixels an em: `size = height * unitsPerEm / (winAscent + winDescent)`. Glyphs rasterized without antialiasing (std's `Canvas.Antialias = False`); GDI hints TrueType glyphs, std doesn't, so small sizes differ. |
| `FreeFont font` | frees a font | `font.Free()` | fbengine E3 | Freeing the current font sets the default font (`bbFreeFont`). |
| `FontWidth()` | the current font's widest character | `CurrentFont().MaxWidth()` | fbengine E3 | `tmMaxCharWidth`, not a cell or an average. |
| `FontHeight()` | the current font's height | `CurrentFont().Height()` | fbengine E3 | `tmHeight`. |
| `StringWidth(string$)` | a string's width in the current font | `CurrentFont().TextWidth(text)` | fbengine E3 | The sum of the characters' cells (overhangs included, so italics come out wider than GDI's own layout); a character outside the font's range counts as its default character. |
| `StringHeight(string$)` | a string's height | `CurrentFont().Height()` | fbengine E3 | Always FontHeight: the string is ignored, no line breaks. The migrator emits Height(). |

## Movies

Blitz plays movies through DirectShow (`gxGraphics::openMovie`, AVI, MPEG, WMV, whatever codecs the
PC had), its sound through DirectShow's default renderer.

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `OpenMovie(file$)` | opens and starts a movie | `Movie.Open(path) Throws IOError` | fbengine_video (new) | Starts playing at once, sound included. Returns 0 on failure. Design question 8. |
| `DrawMovie(movie[, x][, y][, w][, h])` | draws the current frame, returns whether it's playing | `movie.Draw(x, y, width, height): Bool` | fbengine_video (new) | w, h default to the movie's size (-1). Stretched into (x, y, w, h) of the current buffer in absolute coordinates: no origin, no viewport clip (`gxMovie::draw`). Returns False, and draws nothing, once it's ended. |
| `MovieWidth(movie)` | its width | `movie.Width()` | fbengine_video (new) | |
| `MovieHeight(movie)` | its height | `movie.Height()` | fbengine_video (new) | |
| `MoviePlaying(movie)` | whether it's still playing | `movie.Playing()` | fbengine_video (new) | Only goes False when a DrawMovie finds the end. |
| `CloseMovie movie` | stops and frees it | `movie.Free()` | fbengine_video (new) | |

## Images: making and freeing

A Blitz image is a list of frames, each its own surface with its own handle and mask, though
HandleImage, MidHandle, and MaskImage set every frame and the size queries read frame 0
(`bbImage`). 2D images have no alpha: LoadImage sets each pixel's alpha to 255 (`loadSurface`
without CANVAS_TEX_ALPHA), and transparency is the mask color alone, black by default
(`gxCanvas` constructor: `setMask( 0 )`).

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `LoadImage(file$)` | loads an image | `Image.Load(path) Throws IOError` | fbengine E3 | Decoded by std.image (`Image.Load`; PNG, JPEG, BMP, TGA, GIF, QOI, WebP); Blitz's FreeImage also read PCX, IFF, and .dds, which the converter turns into PNG. Alpha discarded, mask black, handle 0,0 or the middle under AutoMidHandle. Returns 0 on failure. An `ImageOptions(alpha: True)` for new code keeps alpha (Design question 5). |
| `LoadAnimImage(file$, width, height, first, count)` | loads a strip of frames | `Image.LoadFrames(path, cellWidth, cellHeight, first, count) Throws IOError` | fbengine E3 | Cells left to right, then top to bottom; partial cells at the right and bottom are ignored (`fpr = width / cellWidth`). count < 1, first < 0, and first + count past the cells are runtime errors ("Illegal frame count", "Illegal first frame", "Not enough frames in bitmap"); fbengine panics. |
| `CopyImage(image)` | copies an image | `image.Copy()` | fbengine E3 | Keeps the handle and the mask (AutoMidHandle doesn't apply). |
| `CreateImage(width, height[, frames])` | makes a blank image | `Image(width, height, frames)` | fbengine E3 | Each frame cleared to black, which is the mask, so it starts transparent. AutoMidHandle applies. frames 0 makes an image with none, which any use then breaks; fbengine panics on frames < 1. |
| `FreeImage image` | frees an image | `image.Free()` | fbengine E3 | If a frame is the current buffer, SetBuffer goes to the FrontBuffer (resetting origin and viewport). Freeing a freed image does nothing in Blitz; fbengine's generational handle makes it a clear error. |
| `SaveImage(image, file$[, frame])` | saves a frame | `image.Save(path, frame, ImageFormat.BMP) Throws IOError` | fbengine E3 | 24-bit BMP whatever the extension, as SaveBuffer. |
| `GrabImage image, x, y[, frame]` | copies part of the current buffer into a frame | `image.Grab(x, y, frame)` | fbengine E3 | The frame's size of the current buffer, from `(x + origin - handle)`, so grabbing where you'd draw it gives back what's there; a solid copy; resets the frame's viewport to the whole frame (`bbGrabImage`). |
| `ImageBuffer(image[, frame])` | a frame's buffer | `image.Buffer(frame): DrawBuffer` | fbengine E3 | Draws into the frame. A frame drawn on the GPU is read back when it's next collided with or pixel-read. |
| `AutoMidHandle enable` | makes new images' handles their middle | `Image.SetAutoMidHandle(enable)` | fbengine E3 | Global, off at start; applies to LoadImage, LoadAnimImage (each frame), and CreateImage, not to CopyImage or the transforms. |

## Images: drawing

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `DrawImage image, x, y[, frame]` | draws a frame, masked | `image.Draw(x, y, frame)` | fbengine E3 | At `(x + origin - handle)`, clipped by the viewport; pixels whose RGB is exactly the mask color are skipped (DirectDraw's source color key). fbengine: a nearest-sampled quad at integer pixels whose shader discards texels equal to the mask, so MaskImage needs no re-upload. |
| `DrawBlock image, x, y[, frame]` | draws a frame, unmasked | `image.DrawBlock(x, y, frame)` | fbengine E3 | As DrawImage, mask ignored. |
| `TileImage image[, x][, y][, frame]` | tiles a frame over the viewport, masked | `image.Tile(x, y, frame)` | fbengine E3 | Covers the whole viewport with tiles placed so one lands at `(x, y)` after origin and handle (port `tile()`'s modulo arithmetic as is, negative offsets included). |
| `TileBlock image[, x][, y][, frame]` | tiles, unmasked | `image.TileBlock(x, y, frame)` | fbengine E3 | |
| `DrawImageRect image, x, y, rect_x, rect_y, rect_width, rect_height[, frame]` | draws part of a frame, masked | `image.DrawRegion(x, y, regionX, regionY, regionWidth, regionHeight, frame)` | fbengine E3 | The part lands at `(x + origin - handle)`: the whole image's handle is subtracted, not offset by the region, so a mid-handled image's parts shift. |
| `DrawBlockRect image, x, y, rect_x, rect_y, rect_width, rect_height[, frame]` | draws part of a frame, unmasked | `image.DrawBlockRegion(x, y, regionX, regionY, regionWidth, regionHeight, frame)` | fbengine E3 | As DrawImageRect. |
| `MaskImage image, red, green, blue` | sets the transparent color | `image.SetMask(Color32(red, green, blue))` | fbengine E3 | Every frame. Black until set. Compared on RGB only. In Blitz, a collision mask already built isn't rebuilt (Behaviors). |

## Images: handles, size, and transforms

The transforms resample each frame into a new one (`tformCanvas`): the new frame is the bounding
box of the transformed corners (floored and ceiled), each pixel the inverse-mapped source pixel,
point-sampled (`floor`) or, with TFormFilter on (the default), bilinear at pixel centers, rounded
to the nearest integer. Samples outside the source give the mask color, so the new area is
transparent and a filtered edge blends toward the mask color (the dark fringe the help warns
of). The handle moves with the image: the new handle is where the old one lands.

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `HandleImage image, x, y` | sets the handle | `image.SetHandle(x, y)` | fbengine E3 | Every frame. |
| `MidHandle image` | puts the handle in the middle | `image.MidHandle()` | fbengine E3 | `(width / 2, height / 2)`, integer division, each frame. |
| `ImageWidth(image)` | width | `image.Width()` | fbengine E3 | Frame 0's. |
| `ImageHeight(image)` | height | `image.Height()` | fbengine E3 | Frame 0's. |
| `ImageXHandle(image)` | handle's x | `image.Handle().x` | fbengine E3 | Frame 0's; returns a `Vec2i`. Changes after a transform. |
| `ImageYHandle(image)` | handle's y | `image.Handle().y` | fbengine E3 | |
| `ScaleImage image, xscale#, yscale#` | scales | `image.Scale(xScale, yScale)` | fbengine E3 | `TFormImage xscale, 0, 0, yscale`. Negative mirrors, and the handle follows: `ScaleImage img, -1, 1` on a 32-wide image with handle 0 gives handle (32, 0), so it draws to the left of x. 0 or a non-finite scale breaks Blitz (a zero-size surface); fbengine panics. |
| `ResizeImage image, width#, height#` | resizes | `image.Resize(width, height)` | fbengine E3 | Scales by `width / frame 0's width`; the result's size comes from floor and ceil of the scaled corners, so a fractional size can come out a pixel off. |
| `RotateImage image, angle#` | rotates | `image.Rotate(degrees)` | fbengine E3 | Degrees, clockwise on screen (y down) for positive angles: `x' = cos θ x - sin θ y`, `y' = sin θ x + cos θ y`, about the handle. Slow in Blitz; fbengine does it on the CPU the same way, and new code uses the batched sprite API's rotation instead. |
| `TFormImage image, a#, b#, c#, d#` | applies a 2x2 matrix | `image.Transform(a, b, c, d)` | fbengine E3 | `x' = a x + c y`, `y' = b x + d y` (`m[0][0]=a, m[1][0]=b, m[0][1]=c, m[1][1]=d`), about the handle. A singular matrix divides by zero in Blitz; fbengine panics. If a frame is the current buffer, SetBuffer goes to the FrontBuffer first. Each transform resamples the last result, so repeated rotations blur. |
| `TFormFilter enable` | bilinear filtering for the transforms | `Image.SetTransformFilter(enable)` | fbengine E3 | Global, on at start (`graphics_create`: `filter=true`), though the help's examples turn it off. Also applies to LoadBuffer's scaling. Each transform Method also takes `filter` defaulting to the global. |

## Image collisions

None of these use the origin or the viewport: positions are what DrawImage would be given, minus
each image's handle. The pixel tests use a bitmask per frame of the pixels whose RGB isn't the
mask color (`updateBitMask`), built on first use and kept up to date as the frame changes.

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `ImagesOverlap(image1, x1, y1, image2, x2, y2)` | whether two images' rectangles overlap | `image1.Overlaps(x1, y1, image2, x2, y2)` | fbengine E3 | Frame 0 of each, handles subtracted; touching edges don't overlap. |
| `ImagesCollide(image1, x1, y1, frame1, image2, x2, y2, frame2)` | whether two frames' unmasked pixels overlap | `image1.Collides(x1, y1, frame1, image2, x2, y2, frame2)` | fbengine E3 | Rectangle test, then the bitmasks ANDed over the intersection (`gxCanvas::collide`). Kept on the CPU per frame; a frame drawn into on the GPU is read back once when its mask is next needed. |
| `RectsOverlap(x1, y1, width1, height1, x2, y2, width2, height2)` | whether two rectangles overlap | `RectsOverlap(Rect(x1, y1, width1, height1), Rect(x2, y2, width2, height2))` | fbengine E3 | Strict: `x1 + w1 <= x2` etc. means no overlap, so touching rectangles don't. std's `Rect.Intersects` counts touching edges, so the migrator must not use it. |
| `ImageRectOverlap(image, x, y, rect_x, rect_y, rect_width, rect_height)` | whether an image's rectangle overlaps a rectangle | `image.OverlapsRect(x, y, Rect(rectX, rectY, rectWidth, rectHeight))` | fbengine E3 | Frame 0, strict, the rect in absolute coordinates. |
| `ImageRectCollide(image, x, y, frame, rect_x, rect_y, rect_width, rect_height)` | whether a frame's unmasked pixels touch a rectangle | `image.CollidesRect(x, y, frame, Rect(rectX, rectY, rectWidth, rectHeight))` | fbengine E3 | Bitmask over the intersection (`rect_collide`). |

## The text console and the pointer

Blitz's Print, Write, and Input always draw on the FrontBuffer, whatever the current buffer, at a
cursor of their own, at origin 0 and the whole buffer, in the current font and color
(`startPrinting`); when the next line would go past the bottom, the buffer scrolls up and the
freed rows are filled with ClsColor. A Blitz program that never calls Graphics still has a
400x300 window (`graphics_create`), so Print there is graphical too.

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `Write string$` | prints without a new line | `Screen.Write(text)` | fbengine E3 | Cursor moves right by StringWidth. In a program with no Graphics, std's `Write` (Design question 9). |
| `Print [string$]` | prints a line | `Screen.Print(text)` | fbengine E3 | Cursor to the next line's start (down by FontHeight), scrolling as above. std's `Print` when there's no Graphics. |
| `Input$([prompt$])` | reads a line from the keyboard | `Screen.Input(prompt): String` | fbengine E3 | Blocks in its own loop (`bbInput`): cursor blinking every 320 ms as an inverted ClsColor block, key repeat after 280 ms then every 40 ms, Backspace, Delete, Home, End, Left, Right, Escape clears the line, Enter ends; only characters the font has are taken; a closed window returns "". fbengine runs Flip inside it, from E1's TextInput. std's `Input` when there's no Graphics. |
| `Locate x, y` | moves the Print cursor | `Screen.Locate(x, y)` | fbengine E3 | Clamped to 0..the FrontBuffer's width and height. SetBuffer resets the cursor to 0,0. |
| `ShowPointer` | shows the mouse pointer | `Mouse.SetVisible(True)` | fbengine E1 | E1 step 4's `Mouse.Visible`, as a Method by the API rules. No effect in Blitz's fullscreen, where the pointer is always hidden. |
| `HidePointer` | hides it | `Mouse.SetVisible(False)` | fbengine E1 | |

## Behaviors to match exactly

1. **Front and back buffer are one surface in a window, kept across Flip.** `gxruntime.cpp`
   `openWindowedGraphics` (`gxGraphics( this,dd,fs,fs,d3d )`), `paint`, `flip`. Games that don't
   Cls every frame rely on it; fbengine's present copies and never discards.
2. **Graphics resets the pen.** `bbgraphics.cpp` `graphics`, `bbEndGraphics`, `graphics_create`:
   color white, ClsColor black, the default font, SetBuffer to the FrontBuffer (2D) or BackBuffer
   (3D).
3. **SetBuffer resets the buffer's origin, viewport, and the Print cursor**, every call.
   `bbSetBuffer`.
4. **Origin applies to positions, not the viewport; Cls fills the viewport only.**
   `gxCanvas::plot`, `rect`, `blit`, `setViewport`, `cls`.
5. **Color packs without masking**, and alpha is forced to 255. `bbColor`, `bbClsColor`,
   `gxCanvas::setColor`.
6. **Black text draws as RGB(0, 0, 16).** `gxFont::render`
   (`if( !(t_canvas->getColor()&0xffffff) ) t_canvas->setColor( 0x10 )`).
7. **Text's cell layout**: top-left at (x, y), each character's cell is its extent plus negative
   A and C overhangs, the glyph drawn `-A` in, no kerning, aliased, chars outside the font's
   range drawn as its default char. `gxGraphics::loadFont`, `gxFont::getWidth`, `gxCanvas::text`.
   Centering subtracts integer halves. `bbText`.
8. **FontWidth is the widest character, StringHeight is FontHeight.** `bbFontWidth`,
   `bbStringHeight`, `gxFont::getWidth()`.
9. **The default font is "courier" at 12, aliased.** `gxGraphics` constructor.
10. **Masks**: black by default, compared on RGB, alpha discarded on load, new images cleared to
    black. `gxCanvas` constructor, `ddUtil::loadSurface`, `gxGraphics::createCanvas`.
11. **Handles**: DrawImage, DrawImageRect, TileImage, CopyRect (from an image's buffer), and
    the collisions subtract the source's handle; GrabImage adds origin minus handle.
    `gxCanvas::blit`, `collide`, `rect_collide`, `bbGrabImage`, `tile`.
12. **MidHandle and AutoMidHandle use integer halves; AutoMidHandle covers Load, LoadAnim, and
    Create, not Copy.** `bbMidHandle`, `bbLoadImage`, `bbLoadAnimImage`, `bbCreateImage`,
    `bbCopyImage`.
13. **LoadAnimImage's frame order and checks.** `bbLoadAnimImage`.
14. **The transforms' resampling**: bounding box by floor and ceil, inverse mapping, bilinear at
    pixel centers with `+ .5` rounding when filtering (the default), floor sampling otherwise,
    the mask color outside the source, the handle remapped. `tformCanvas`, `getPixel`
    (bbgraphics.cpp), `bbTFormImage`, `bbRotateImage` (degrees negated, clockwise on screen),
    `bbResizeImage` (frame 0's size), `graphics_create` (`filter=true`).
15. **The pixel-perfect test**: rectangles first (touching is no hit), then the masks of
    non-mask RGB pixels ANDed over the intersection. `gxCanvas::collide`, `rect_collide`,
    `updateBitMask`. Blitz's MaskImage doesn't rebuild a mask already built (`setMask` doesn't
    touch `cm_mask`), so collisions after a mask change see the old mask until the frame's drawn
    on; nothing sane relies on it, so fbengine rebuilds (a fix, not a match).
16. **RectsOverlap and ImagesOverlap are strict.** `bbRectsOverlap`, `gxCanvas::collide`.
17. **ReadPixel outside the viewport returns the mask color, with alpha 255; the Fast
    commands ignore origin and viewport.** `gxCanvas::getPixel`, `getPixelFast`,
    `PixelFormat::toARGB` (`argbfill`).
18. **Line is Bresenham with both ends, clipped by integer division first; a zero-length line
    adds the origin twice.** `gxCanvas::line`.
19. **Oval's off-center, row-by-row spans.** `gxCanvas::oval`.
20. **Rect's outline is four clipped strips; non-positive sizes draw nothing.** `gxCanvas::rect`,
    `clip`.
21. **TileImage's coverage and offset arithmetic.** `tile` (bbgraphics.cpp).
22. **LoadBuffer scales with the transform filter and leaves the origin at 0,0;
    SaveBuffer/SaveImage write 24-bit BMP.** `bbLoadBuffer`, `saveCanvas`.
23. **Freeing or transforming the current buffer's image sets the FrontBuffer.** `bbFreeImage`,
    `bbTFormImage`.
24. **FreeFont of the current font sets the default.** `bbFreeFont`.
25. **VWait waits once whatever its argument.** `bbVWait`.
26. **Print, Write, and Input on the FrontBuffer with their own cursor, scrolling with
    ClsColor; Input's keys and timings.** `startPrinting`, `endPrinting`, `bbInput`, `bbLocate`.
27. **The pointer is hidden in fullscreen whatever ShowPointer says.** `gxRuntime::openGraphics`
    (`ShowCursor( 0 )`), `setPointerVisible`.
28. **Graphics' mode numbers, mode 0 by build, and the error on others.** `bbGraphics`,
    `bbGraphics3D`.
29. **Float arguments to these Int parameters are rounded to nearest, ties to even** (x87
    `fistp`, `compiler/codegen_x86/codegen_x86.cpp`), so `DrawImage img, 10.5, 0` draws at 10.
    The migrator emits that rounding, not FB's own conversion.
30. **Gamma: three independent entries, clamped 0..255, stored as WORDs of `* 257`.**
    `bbSetGamma`, `gxGraphics::setGamma`, `getGamma`.

## Design questions

1. **The pen as Functions or as a buffer's Methods?** Blitz's drawing state is global and
   SetBuffer's resets are part of its behavior. Recommendation: Functions on the current buffer
   (`SetBuffer`, `SetDrawColor`, `DrawRect`, `DrawText`), the "app-level singleton" case of the
   API rules, with images' and buffers' own operations as Methods (`image.Draw`,
   `buffer.ReadPixel`). New code wanting objects gets the batched sprite API E3 also has.
2. **What GraphicsWidth means on high DPI and in scaled windows.** E1's Graphics takes desktop
   units; Blitz's takes pixels, and mode 3 stretches a fixed buffer. Recommendation: the screen
   buffer is exactly the pixels Graphics asks for; the window is that many desktop units; the
   buffer is scaled to the window's pixels (nearest, integer when it fits, letterboxed:
   `Screen.SetScaling(ScreenScaling.PIXEL_PERFECT | STRETCH | LINEAR)`), and MouseX and MouseY
   are in buffer pixels, as Blitz's `moveMouse` scaled them in mode 3.
3. **Int or Float coordinates.** Recommendation: Int32 for these Blitz-shaped commands, so pixels
   match; the batched sprite API takes Floats and Vec2. The migrator emits Blitz's rounding.
4. **Programs that draw on the FrontBuffer and never Flip** (`Graphics 640,480 : Text 0,0,"x" :
   WaitKey`, and `While Not KeyHit(1) : Wend`): with input read at Flip they'd never show or end.
   Recommendation: FrontBuffer and BackBuffer are one buffer; the migrator puts `Flip()` in loops
   without one; the blocking calls (`WaitKey`, `WaitMouse`, `Screen.Input`, and `Delay` in a
   graphics program) present and read input as they wait, as Blitz's 100 ms repaint timer did.
5. **Alpha in 2D images.** Blitz throws PNG alpha away. Recommendation: `Image.Load` matches
   Blitz (alpha off, mask black); `ImageOptions(alpha: True)` and a blend mode are for new code;
   the migrator never sets it.
6. **Graphics mode 0** (windowed in debug, fullscreen in release). Recommendation: the migrator
   emits WINDOWED and a comment, since today's players expect a window and debug builds were
   windowed; a game that wants fullscreen says 1.
7. **System fonts.** LoadFont("Arial") needs a lookup by family on Windows, Linux, and macOS, and
   the default is the raster Courier, which can't be shipped. Recommendation: fbengine searches
   the OS's font folders by family (std.image's `Font` reads the files; new in std:
   `Font.FamilyName()` and `Font.StyleName()` from the name table), falls back to bundled
   metric-compatible faces (Liberation Sans, Serif, Mono for Arial, Times New Roman, Courier New),
   and ships an OFL bitmap font close to raster Courier 12 as the default.
8. **Movies.** DirectShow is gone, and fbengine can't call fbengine_audio. Recommendation: a
   `fbengine_video` package after E9 on pl_mpeg (MPEG-1 video and MP2 audio, public domain) that
   uses both fbengine's and fbengine_audio's FB APIs, with the cooker converting AVI and WMV to
   MPEG-1; or drop movies, the migrator emitting a comment, if no target game uses them. Not in
   E0-E14 today.
9. **Print without Graphics.** Recommendation: a program that never calls Graphics gets std's
   `Print`, `Write`, and `Input` (a console tool now); one that does gets `Screen.Print`.
10. **Should `Screen.Input` step `Scheduler.Main`?** Blitz had no coroutines, so either is
    faithful. Recommendation: yes, through Flip, so a game's music fades and timers keep going.
11. **Name clashes with std.image**: fbengine's `Image` and `Font` against std.image's `Image`,
    `Font`, and `Canvas`, and `DrawBuffer` chosen to miss std.ffi's `Buffer<T>` and Blitz's 3D
    `Surface`. Recommendation: keep `Image` and `Font` (what Blitz users look for); fbengine's
    own code and a game that needs both qualify std's (`image.Image`), and `image.ToStdImage()` and
    `Image.From(stdImage)` convert.
12. **Gamma when windowed.** Blitz ignored it in a window. Recommendation: apply it always, as a
    present-pass LUT; a game's brightness slider then works in both.
13. **Bug-for-bug items** (behaviors 15, 18, 22: the stale collision mask, the doubled origin of a
    zero-length line, LoadBuffer's lost origin). Recommendation: match 18 and 22 (cheap, visible
    in golden images), fix 15.

## Counts

111 commands mapped, none skipped (the list has no internal `_` names).

| Where | Commands |
|---|---|
| fbengine E1 | 16: CountGfxDrivers, GfxDriverName, SetGfxDriver, CountGfxModes, GfxModeExists, GfxModeWidth, GfxModeHeight, CountGfxModes3D, GfxMode3DExists, Graphics, Graphics3D, EndGraphics, VWait, Flip, ShowPointer, HidePointer (of which SetGfxDriver and VWait need additions to E1) |
| fbengine E2 | 2: TotalVidMem, AvailVidMem |
| fbengine E3 | 79: gamma (5), FrontBuffer, BackBuffer, GraphicsWidth, GraphicsHeight, SetBuffer, GraphicsBuffer, LoadBuffer, SaveBuffer, pixel access (8), the pen (10), primitives and CopyRect (6), fonts (6), images (27), collisions (5), Write, Print, Input, Locate |
| fbengine_video (new) | 6: OpenMovie, DrawMovie, MovieWidth, MovieHeight, MoviePlaying, CloseMovie |
| std (new) | 0 commands; supporting additions: `Color32.FromArgb` and `ToArgb`, `Font.FamilyName` and `StyleName` |
| dropped | 8: GfxModeDepth, GfxDriver3D, GfxMode3D, Windowed3D, GraphicsLost, ScanLine, GraphicsDepth, BufferDirty |
