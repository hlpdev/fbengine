# Blitz3D to FutureBlitz: io (input, audio, sockets, DirectPlay)

Covers every command in b3d-commands/bbinput.txt (41), bbaudio.txt (17), bbsockets.txt (20), and
multiplay.txt (14). Sources read: bbruntime/bbinput.cpp, bbaudio.cpp, bbsockets.cpp, multiplay.cpp,
multiplay_setup.cpp; gxruntime/gxinput.cpp, gxdevice.cpp, gxruntime.cpp (windowProc, idle,
suspend, moveMouse), gxaudio.cpp, gxsound.cpp, gxmusic.cpp; blitz3d/listener.cpp, object.cpp
(emitSound, updateSounds, velocity); the help pages; fbengine's packages/fbengine/*.fb (window,
display, frame, simulate: no input yet); std/net/*.fb and std/net/game/*.fb.

Where `fbengine E13` appears, it means a small compat shim the migrator (E13) emits or ships for
a Blitz behavior with no clean modern API (ASCII key queues, DirectPlay); the modern API it is
built on is named in the row. See design question 1.

Conventions for the proposed fbengine API follow engine.md: `@MainThread` Functions and Methods,
no Properties; Blitz-like app-level Functions where natural (`KeyHit(Key.SPACE)`); UPPER_CASE
enum members.

## Keyboard

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `KeyDown(key)` | True while the key with that scancode is held | `KeyDown(Key.ESCAPE)` | fbengine E1 | Scancode, by position, not by layout (DIK set, table below). Blitz indexes a 256-entry array with no bounds check; a computed int goes through `n As Key` (see design question 2). Held state comes from key-down/up events, so auto-repeat never matters. |
| `KeyHit(key)` | How many times the key was pressed since the last KeyHit of it (or FlushKeys) | `KeyHit(Key.SPACE)` | fbengine E1 | Blitz returns a count, not a Bool, and zeroes it on read (gxDevice::keyHit), so two calls in one frame split the presses. fbengine counts per Flip (engine.md); the migrator keeps one call per key per frame. `If KeyHit(k)` works the same since the count is truthy. Auto-repeat is ignored (windowProc drops WM_KEYDOWN with bit 30 set). |
| `GetKey()` | Pops the next key press from a 32-deep queue and returns its ASCII code, or 0 | `BlitzGetKey()`, over `Input.Presses()` and `KeyChar(key, shift)` (both proposed for E1 step 3); modern code uses `TextInput()` | fbengine E13 | Queue is shared by all key presses, ASCII or not: a press with no ASCII pops and returns 0. Queue persists across frames until read and drops presses past 32. ASCII is ToAscii of the scancode with only Shift/Ctrl/Alt state: Caps Lock and Num Lock are ignored. Insert, Delete, Home, End, PgUp, PgDn give 3, 4, 1, 2, 5, 6; arrows give Up 28, Down 29, Right 30, Left 31 (gxInput::toAscii). Characters are the ANSI code page's byte (`ch & 255`). |
| `WaitKey()` | Waits (pumping messages, 20 ms sleeps) for a key press with an ASCII code and returns it | `BlitzWaitKey()`, over `WaitInput()` (proposed, E1 step 3) | fbengine E13 | Skips non-ASCII presses. Does not Flip, so the screen is unchanged. Closing the window during the wait ends the program (RTEX(0)). |
| `FlushKeys` | Clears the press counts and the key queue | `FlushKeys()` | fbengine E1 | Held state is kept: KeyDown stays True for a held key; only KeyHit and GetKey are cleared. In fbengine it zeroes this frame's counts until the next Flip. |

### Scancodes: Blitz's codes to `Key`

Blitz's codes are DirectInput's DIK_ values (help: scancodes.htm). In windowed mode (the default:
DirectInput is used only in fullscreen after `EnableDirectInput True`) the code is built from
WM_KEYDOWN's lParam as `scan | (extended << 7)`, which gives the same numbers except where noted.
Proposed `Key` member values equal Blitz's codes (design question 2).

| Blitz | Key (help) | `Key.*` | SDL3 scancode | Notes |
|---|---|---|---|---|
| 1 | Escape | `ESCAPE` | `SDL_SCANCODE_ESCAPE` | |
| 2-10 | 1-9 | `DIGIT_1` ... `DIGIT_9` | `SDL_SCANCODE_1` ... `_9` | main row |
| 11 | 0 | `DIGIT_0` | `SDL_SCANCODE_0` | |
| 12 | Minus | `MINUS` | `SDL_SCANCODE_MINUS` | |
| 13 | Equals | `EQUALS` | `SDL_SCANCODE_EQUALS` | |
| 14 | Backspace | `BACKSPACE` | `SDL_SCANCODE_BACKSPACE` | |
| 15 | Tab | `TAB` | `SDL_SCANCODE_TAB` | |
| 16-25 | Q W E R T Y U I O P | `Q` ... `P` | `SDL_SCANCODE_Q` ... | by position: AZERTY's A key is `Key.Q` |
| 26 | [ | `LEFT_BRACKET` | `SDL_SCANCODE_LEFTBRACKET` | |
| 27 | ] | `RIGHT_BRACKET` | `SDL_SCANCODE_RIGHTBRACKET` | |
| 28 | Return | `RETURN` | `SDL_SCANCODE_RETURN` | main keyboard |
| 29 | Left Control | `LEFT_CONTROL` | `SDL_SCANCODE_LCTRL` | |
| 30-38 | A S D F G H J K L | `A` ... `L` | `SDL_SCANCODE_A` ... | |
| 39 | ; | `SEMICOLON` | `SDL_SCANCODE_SEMICOLON` | |
| 40 | ' | `APOSTROPHE` | `SDL_SCANCODE_APOSTROPHE` | |
| 41 | Grave | `GRAVE` | `SDL_SCANCODE_GRAVE` | |
| 42 | Left Shift | `LEFT_SHIFT` | `SDL_SCANCODE_LSHIFT` | |
| 43 | Backslash | `BACKSLASH` | `SDL_SCANCODE_BACKSLASH` | on ISO boards this is the #~ key; SDL reports it as BACKSLASH too (NONUSHASH is rarely generated) |
| 44-50 | Z X C V B N M | `Z` ... `M` | `SDL_SCANCODE_Z` ... | |
| 51 | , | `COMMA` | `SDL_SCANCODE_COMMA` | |
| 52 | . | `PERIOD` | `SDL_SCANCODE_PERIOD` | |
| 53 | / | `SLASH` | `SDL_SCANCODE_SLASH` | |
| 54 | Right Shift | `RIGHT_SHIFT` | `SDL_SCANCODE_RSHIFT` | |
| 55 | Keypad * | `KEYPAD_MULTIPLY` | `SDL_SCANCODE_KP_MULTIPLY` | |
| 56 | Left Alt | `LEFT_ALT` | `SDL_SCANCODE_LALT` | arrives as WM_SYSKEYDOWN; Blitz handles it |
| 57 | Space | `SPACE` | `SDL_SCANCODE_SPACE` | |
| 58 | Caps Lock | `CAPS_LOCK` | `SDL_SCANCODE_CAPSLOCK` | |
| 59-68 | F1-F10 | `F1` ... `F10` | `SDL_SCANCODE_F1` ... | F10 arrives as WM_SYSKEYDOWN, handled |
| 69 | Num Lock | `NUM_LOCK` | `SDL_SCANCODE_NUMLOCKCLEAR` | windowed trap: Windows gives Pause as 0x45 without the extended bit and Num Lock as 0x45 with it, so windowed Blitz most likely sees Pause as 69 and Num Lock as 197, the reverse of the table. Check on Windows before E13 decides which the migrator honors |
| 70 | Scroll Lock | `SCROLL_LOCK` | `SDL_SCANCODE_SCROLLLOCK` | |
| 71-73 | Keypad 7 8 9 | `KEYPAD_7` ... `KEYPAD_9` | `SDL_SCANCODE_KP_7` ... | Blitz ignores Num Lock: the same code whether it's on or not |
| 74 | Keypad - | `KEYPAD_SUBTRACT` | `SDL_SCANCODE_KP_MINUS` | |
| 75-77 | Keypad 4 5 6 | `KEYPAD_4` ... `KEYPAD_6` | `SDL_SCANCODE_KP_4` ... | |
| 78 | Keypad + | `KEYPAD_ADD` | `SDL_SCANCODE_KP_PLUS` | |
| 79-81 | Keypad 1 2 3 | `KEYPAD_1` ... `KEYPAD_3` | `SDL_SCANCODE_KP_1` ... | |
| 82 | Keypad 0 | `KEYPAD_0` | `SDL_SCANCODE_KP_0` | |
| 83 | Keypad . | `KEYPAD_DECIMAL` | `SDL_SCANCODE_KP_PERIOD` | |
| 86 | OEM_102 (UK/DE <>) | `NON_US_BACKSLASH` | `SDL_SCANCODE_NONUSBACKSLASH` | |
| 87, 88 | F11, F12 | `F11`, `F12` | `SDL_SCANCODE_F11`, `_F12` | |
| 100-102 | F13-F15 (PC98) | `F13` ... `F15` | `SDL_SCANCODE_F13` ... | also on modern keyboards with F13+ |
| 112 | Kana | `KANA` | `SDL_SCANCODE_INTERNATIONAL2` | Japanese |
| 115 | ABNT_C1 (/?) | `ABNT_C1` | `SDL_SCANCODE_INTERNATIONAL1` | Brazilian; JP Ro shares HID usage |
| 121 | Convert | `CONVERT` | `SDL_SCANCODE_INTERNATIONAL4` | Japanese |
| 123 | NoConvert | `NO_CONVERT` | `SDL_SCANCODE_INTERNATIONAL5` | Japanese |
| 125 | Yen | `YEN` | `SDL_SCANCODE_INTERNATIONAL3` | Japanese |
| 126 | ABNT_C2 (keypad .) | `ABNT_C2` | `SDL_SCANCODE_KP_COMMA` | Brazilian keypad; collides with 179 below |
| 141 | Keypad = (PC98) | `KEYPAD_EQUALS` | `SDL_SCANCODE_KP_EQUALS` | |
| 144 | Previous Track | `MEDIA_PREVIOUS` | `SDL_SCANCODE_MEDIA_PREVIOUS_TRACK` | DIK_CIRCUMFLEX on JP boards in DirectInput |
| 145-147 | AT, Colon, Underline (PC98) | none | none | NEC PC98 only; dropped, migrator leaves a comment |
| 148 | Kanji | `KANJI` | `SDL_SCANCODE_LANG2`? | uncertain mapping on modern JP boards; check with a JP keyboard |
| 149 | Stop (PC98) | `STOP` | `SDL_SCANCODE_STOP` | |
| 150, 151 | AX, Unlabeled (J3100) | none | none | dropped |
| 153 | Next Track | `MEDIA_NEXT` | `SDL_SCANCODE_MEDIA_NEXT_TRACK` | |
| 156 | Keypad Enter | `KEYPAD_ENTER` | `SDL_SCANCODE_KP_ENTER` | |
| 157 | Right Control | `RIGHT_CONTROL` | `SDL_SCANCODE_RCTRL` | |
| 160 | Mute | `MUTE` | `SDL_SCANCODE_MUTE` | |
| 161 | Calculator | none | none in SDL3 | SDL3 dropped CALCULATOR; dropped |
| 162 | Play/Pause | `MEDIA_PLAY_PAUSE` | `SDL_SCANCODE_MEDIA_PLAY_PAUSE` | |
| 164 | Media Stop | `MEDIA_STOP` | `SDL_SCANCODE_MEDIA_STOP` | |
| 174 | Volume Down | `VOLUME_DOWN` | `SDL_SCANCODE_VOLUMEDOWN` | |
| 176 | Volume Up | `VOLUME_UP` | `SDL_SCANCODE_VOLUMEUP` | |
| 178 | Web Home | `BROWSER_HOME` | `SDL_SCANCODE_AC_HOME` | |
| 179 | Keypad , (PC98) | `KEYPAD_COMMA` | `SDL_SCANCODE_KP_COMMA` | SDL maps 126 and 179 to one scancode; the native layer reports 126 (more common) |
| 181 | Keypad / | `KEYPAD_DIVIDE` | `SDL_SCANCODE_KP_DIVIDE` | |
| 183 | SysRq / Print Screen | `PRINT_SCREEN` | `SDL_SCANCODE_PRINTSCREEN` | windowed trap: Windows sends no WM_KEYDOWN for Print Screen, only WM_KEYUP, so windowed Blitz never sees it held or hit |
| 184 | Right Alt | `RIGHT_ALT` | `SDL_SCANCODE_RALT` | AltGr on many layouts also sends Left Control (29) in Windows; SDL doesn't |
| 197 | Pause | `PAUSE` | `SDL_SCANCODE_PAUSE` | see 69 |
| 199 | Home | `HOME` | `SDL_SCANCODE_HOME` | |
| 200 | Up | `UP` | `SDL_SCANCODE_UP` | |
| 201 | Page Up | `PAGE_UP` | `SDL_SCANCODE_PAGEUP` | |
| 203 | Left | `LEFT` | `SDL_SCANCODE_LEFT` | |
| 205 | Right | `RIGHT` | `SDL_SCANCODE_RIGHT` | |
| 207 | End | `END` | `SDL_SCANCODE_END` | |
| 208 | Down | `DOWN` | `SDL_SCANCODE_DOWN` | |
| 209 | Page Down (Next) | `PAGE_DOWN` | `SDL_SCANCODE_PAGEDOWN` | |
| 210 | Insert | `INSERT` | `SDL_SCANCODE_INSERT` | |
| 211 | Delete | `DELETE` | `SDL_SCANCODE_DELETE` | |
| 219 | Left Windows | `LEFT_SUPER` | `SDL_SCANCODE_LGUI` | Command on macOS |
| 220 | Right Windows | `RIGHT_SUPER` | `SDL_SCANCODE_RGUI` | |
| 221 | Apps | `MENU` | `SDL_SCANCODE_APPLICATION` | |
| 222 | Power | `POWER` | `SDL_SCANCODE_POWER` | |
| 223 | Sleep | `SLEEP` | `SDL_SCANCODE_SLEEP` | |
| 227 | Wake | `WAKE` | `SDL_SCANCODE_WAKE` | |
| 229 | Web Search | `BROWSER_SEARCH` | `SDL_SCANCODE_AC_SEARCH` | |
| 230 | Web Favorites | `BROWSER_FAVORITES` | `SDL_SCANCODE_AC_BOOKMARKS` | |
| 231 | Web Refresh | `BROWSER_REFRESH` | `SDL_SCANCODE_AC_REFRESH` | |
| 232 | Web Stop | `BROWSER_STOP` | `SDL_SCANCODE_AC_STOP` | |
| 233 | Web Forward | `BROWSER_FORWARD` | `SDL_SCANCODE_AC_FORWARD` | |
| 234 | Web Back | `BROWSER_BACK` | `SDL_SCANCODE_AC_BACK` | |
| 235 | My Computer | none | none in SDL3 | dropped |
| 236 | Mail | none | none in SDL3 | dropped |
| 237 | Media Select | `MEDIA_SELECT` | `SDL_SCANCODE_MEDIA_SELECT` | |

Keys Blitz never had (F16-F24, Non-US #, keypad extras, Left/Right media keys SDL adds) get
`Key` values from 256 up, so they never collide with a Blitz code.

## Mouse

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `MouseDown(button)` | True while the button is held: 1 left, 2 right, 3 middle | `MouseDown(MouseButton.LEFT)` | fbengine E1 | Blitz reads only three buttons (DIMOUSESTATE's first three, WM_L/R/MBUTTON); X1/X2 never register. Migrator maps 1, 2, 3 to LEFT, RIGHT, MIDDLE; other literals become `False`. |
| `MouseHit(button)` | Presses since the last MouseHit of it | `MouseHit(MouseButton.LEFT)` | fbengine E1 | A count, zeroed on read, as KeyHit. Per frame in fbengine. |
| `GetMouse()` | Pops the next pressed button (1-3) from a 32-deep queue, or 0 | `BlitzGetMouse()`, over `Input.Presses()` | fbengine E13 | Separate queue from the keyboard's. |
| `WaitMouse()` | Waits for a button press and returns it | `BlitzWaitMouse()`, over `WaitInput()` | fbengine E13 | No Flip; closing the window ends the program. |
| `MouseWait()` | Same as WaitMouse | `BlitzWaitMouse()` | fbengine E13 | Registered to the same function (bbWaitMouse). |
| `MouseX()` | The cursor's x in the graphics buffer's pixels | `MouseX()` | fbengine E1 | Clamped to 0..GraphicsWidth-1 (windowProc, Mouse::update). In scaled windowed mode (Graphics mode 3) it's scaled from the window's client size to the graphics size. Outside the window with no button held, it stays at the last position (SetCapture only while a button is down). Returns 0 until the first move, and back to 0 after a suspend (input reset). |
| `MouseY()` | The cursor's y | `MouseY()` | fbengine E1 | As MouseX; down is positive. |
| `MouseZ()` | The wheel's position in notches, from 0 at start | `MouseZ()` | fbengine E1 | Accumulated WHEEL_DELTA units divided by 120 and truncated toward zero (`getAxisState(2)/120`), so high-resolution wheels add up before it changes. Away from the user is positive. Reset to 0 by a suspend. SDL3's wheel `y` is in notches as a float: sum it and truncate. |
| `MouseXSpeed()` | x moved since the last MouseXSpeed call | `MouseXSpeed()` | fbengine E1 | Since the last call, not per frame (a static in bbinput.cpp); position-based, so it reads 0 at a screen edge, which is why Blitz games call `MoveMouse` to the center after it. MoveMouse resets its baseline. fbengine: per frame; the migrator keeps one call per frame. Must not count MoveMouse's own warp as motion. Modern mouse look uses `Mouse.SetRelative(True)`. |
| `MouseYSpeed()` | y moved since the last call | `MouseYSpeed()` | fbengine E1 | As MouseXSpeed. |
| `MouseZSpeed()` | Wheel notches since the last call | `MouseZSpeed()` | fbengine E1 | Difference of MouseZ values, so the 120 truncation applies first. |
| `FlushMouse` | Clears the button press counts and queue | `FlushMouse()` | fbengine E1 | Held state and position are kept. New in E1 step 4 (not in engine.md's list yet). |
| `MoveMouse x,y` | Moves the cursor to x, y in graphics coordinates | `MoveMouse(x, y)` | fbengine E1 | Sets MouseX/Y at once and the speed baselines (`mouse_x=x`). Scaled back to window coordinates in scaled mode. In fullscreen with DirectInput on, only the internal position moves. SDL3: SDL_WarpMouseInWindow, in window coordinates from Graphics coordinates. |

## Joysticks to SDL3 gamepads

Blitz enumerates attached DirectInput joysticks once, at startup (no hot-plugging); `port` is the
0-based index into that list, and any command with a port out of range returns 0 rather than
failing. Axes are normalized as `(v - min) / (max - min) * 2 - 1`, with no dead zone, polled at
most every 3 ms. The range lookup in the Joystick constructor reads each axis's range at offset
`k*4+12` (lRx's onward), off by three axes, but DirectInput's default range is 0..65535 for every
axis, so in practice every axis is `v / 65535 * 2 - 1`, and an axis the device lacks reads 0 raw,
so -1 normalized.

Most players of a migrated game will use an Xbox-style pad, which DirectInput sees through its
HID driver as: X/Y the left stick, Z both triggers (one toward +1, the other toward -1; both held
cancel), Rx/Ry the right stick, the D-pad as POV hat 0, and buttons 1 A, 2 B, 3 X, 4 Y, 5 LB,
6 RB, 7 Back, 8 Start, 9 left stick click, 10 right stick click (no Guide, triggers aren't
buttons). That layout is what the migrator maps onto SDL3's gamepad API, so a game tuned for
that pad behaves the same:

| Blitz | DirectInput (Xbox pad) | SDL3 gamepad | fbengine |
|---|---|---|---|
| JoyX / JoyXDir | X: left stick x | `SDL_GAMEPAD_AXIS_LEFTX` | `pad.RawAxis(GamepadAxis.LEFT_X)` |
| JoyY / JoyYDir | Y: left stick y, down positive | `SDL_GAMEPAD_AXIS_LEFTY` (down positive too) | `pad.RawAxis(GamepadAxis.LEFT_Y)` |
| JoyZ / JoyZDir | Z: triggers combined | `LEFT_TRIGGER - RIGHT_TRIGGER` | `pad.RawAxis(GamepadAxis.LEFT_TRIGGER) - pad.RawAxis(GamepadAxis.RIGHT_TRIGGER)` (sign to check against a real pad) |
| JoyPitch | Rx * 180: right stick x | `SDL_GAMEPAD_AXIS_RIGHTX` | `pad.RawAxis(GamepadAxis.RIGHT_X) * 180` |
| JoyYaw | Ry * 180: right stick y | `SDL_GAMEPAD_AXIS_RIGHTY` | `pad.RawAxis(GamepadAxis.RIGHT_Y) * 180` |
| JoyRoll | Rz * 180: absent, reads -180 | none | `0` and a migrator TODO, or a raw joystick's axis (design question 4) |
| JoyU / JoyV | sliders 0 and 1: absent, read -1 | none | as JoyRoll |
| JoyHat | POV 0 in degrees, -1 centered | D-pad buttons | `BlitzJoyHat(pad)` |
| button 1-4 | A, B, X, Y | `SOUTH`, `EAST`, `WEST`, `NORTH` | `GamepadButton.SOUTH` ... |
| button 5, 6 | LB, RB | `LEFT_SHOULDER`, `RIGHT_SHOULDER` | |
| button 7, 8 | Back, Start | `BACK`, `START` | |
| button 9, 10 | stick clicks | `LEFT_STICK`, `RIGHT_STICK` | |
| button 11-31 | other devices' buttons | none on a gamepad | raw joystick button `n - 1`, or `False` |

`pad` is the gamepad at that Blitz port: the shim's port table assigns ports in connection order
from startup and keeps them (Blitz never renumbers); a port with no gamepad behaves as Blitz's
out-of-range port.

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `JoyType([port])` | 0 none, 1 gamepad ("digital" in the help), 2 joystick ("analog") | `BlitzJoyType(port)`, over `Gamepads()` (and `Joysticks()`, design question 4) | fbengine E13 | 1 when DirectInput's device subtype is GAMEPAD, else 2. An Xbox pad is 1. |
| `JoyDown(button[,port])` | True while button (1-31) is held | `pad.Down(GamepadButton.SOUTH)` | fbengine E1 | Literal button numbers map by the table above; a computed one goes through `BlitzJoyButton(n)`. Only 31 buttons are read (k+1 for k < 31). |
| `JoyHit(button[,port])` | Presses since the last JoyHit of it | `pad.Hit(GamepadButton.SOUTH)` | fbengine E1 | A count zeroed on read; per frame in fbengine. |
| `GetJoy([port])` | Pops the next pressed button number from the port's 32-deep queue, or 0 | `BlitzGetJoy(port)`, over `Input.Presses()` | fbengine E13 | Returns Blitz's DirectInput numbering, translated back from SDL's button. |
| `WaitJoy([port])` | Waits for a button press on the port and returns its number | `BlitzWaitJoy(port)`, over `WaitInput()` | fbengine E13 | Returns 0 at once for a port out of range. |
| `JoyWait([port])` | Same as WaitJoy | `BlitzWaitJoy(port)` | fbengine E13 | Registered to bbWaitJoy. |
| `JoyX#([port])` | Axis X, -1 to 1 | `pad.RawAxis(GamepadAxis.LEFT_X)` | fbengine E1 | No dead zone in Blitz: E1's Axis applies one, so the migrator uses RawAxis (proposed: the axis before the dead zone). 0 until the first poll. Rest position is about -0.00002, not 0 (32767 of 0..65535). |
| `JoyY#([port])` | Axis Y | `pad.RawAxis(GamepadAxis.LEFT_Y)` | fbengine E1 | Down positive, as SDL's. |
| `JoyZ#([port])` | Axis Z | `pad.RawAxis(GamepadAxis.LEFT_TRIGGER) - pad.RawAxis(GamepadAxis.RIGHT_TRIGGER)` | fbengine E1 | Triggers share one axis under DirectInput. |
| `JoyU#([port])` | Slider 0 | `0` (TODO), or a raw joystick's axis | fbengine E1 | Absent on pads: reads -1 in Blitz. Faithful -1 is an artifact; emit 0 with a TODO. |
| `JoyV#([port])` | Slider 1 | as JoyU | fbengine E1 | As JoyU. |
| `JoyPitch#([port])` | Rx * 180, -180 to 180 | `pad.RawAxis(GamepadAxis.RIGHT_X) * 180` | fbengine E1 | Despite the name, on a pad it's the right stick's x. |
| `JoyYaw#([port])` | Ry * 180 | `pad.RawAxis(GamepadAxis.RIGHT_Y) * 180` | fbengine E1 | Right stick's y. |
| `JoyRoll#([port])` | Rz * 180 | `0` (TODO), or a raw joystick's axis | fbengine E1 | Absent on pads: -180 in Blitz. |
| `JoyHat([port])` | POV 0 in whole degrees clockwise from up (0, 45, ... 315), -1 when centered | `BlitzJoyHat(pad)` | fbengine E13 | `floor(pov / 100 + 0.5)` of DirectInput's hundredths. From the D-pad: up 0, up-right 45, right 90, down-right 135, down 180, down-left 225, left 270, up-left 315, none -1; opposite directions held cancel. |
| `JoyXDir([port])` | -1, 0, or 1 for X past a third | `BlitzJoyDir(pad.RawAxis(GamepadAxis.LEFT_X))` | fbengine E13 | `t < -1/3 ? -1 : (t > 1/3 ? 1 : 0)`, strict comparisons. |
| `JoyYDir([port])` | As JoyXDir for Y | `BlitzJoyDir(pad.RawAxis(GamepadAxis.LEFT_Y))` | fbengine E13 | |
| `JoyZDir([port])` | As JoyXDir for Z | `BlitzJoyDir(` the JoyZ expression `)` | fbengine E13 | |
| `JoyUDir([port])` | As JoyXDir for U | `0` (TODO) | fbengine E13 | -1 on pads in Blitz (absent axis). |
| `JoyVDir([port])` | As JoyXDir for V | `0` (TODO) | fbengine E13 | As JoyUDir. |
| `FlushJoy` | Clears every joystick's press counts and queue | `FlushGamepads()` | fbengine E1 | Held state kept. New name for E1 step 5. |

## DirectInput

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `EnableDirectInput enable` | In fullscreen, reads mouse and keyboard through DirectInput (exclusive, relative mouse) rather than window messages | none | dropped | SDL3 reads input one way on every OS. The visible effect (fullscreen mouse integrates raw motion, clamped to the screen) is what `Mouse.SetRelative(True)` gives a mouse-look game. Migrator removes the call, with a comment when it enables it. |
| `DirectInputEnabled()` | Whether DirectInput is in use | `False` | dropped | Constant. Note Blitz turns it off by itself when acquiring fails (`use_di=input->acquire()`). |

## Sounds

fbengine_audio, E9, over miniaudio's engine. Proposed shape: `Sound` (a loaded sample, a
resource with `Free()`), `Channel` (one playing voice, a small generational value), `Music` for
streams, and `Listener` for 3D. Blitz's audio is FMOD 3, initialized at 44100 Hz with up to 1024
voices, with FSOUND_INIT_GLOBALFOCUS: sound keeps playing when the window loses focus (and
gxAudio::pause is empty, so a suspend doesn't pause it either).

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `LoadSound(file$)` | Loads a whole sample into memory, 2D; 0 on failure | `Try Sound.Load(path)` | fbengine_audio E9 | Formats: raw, wav, mp3, ogg (help); miniaudio does wav, flac, mp3, and Vorbis through stb_vorbis. Blitz returns 0 on failure and code tests `If snd = 0`: the migrator wraps it in Try/Catch to Null where the result is tested. Each load is its own copy (no cache); E8's cache is design question 7. |
| `FreeSound sound` | Frees a sound | `snd.Free()` | fbengine_audio E9 | `FreeSound 0` is a no-op. Channels already playing it most likely stop when FSOUND_Sample_Free frees the sample under them (check). In debug, a freed handle is "Sound does not exist"; in release, undefined. |
| `LoopSound sound` | Makes later plays of the sound loop | `snd.SetLoop(True)` | fbengine_audio E9 | Sets the sample's mode at once (FSOUND_Sample_SetMode), so it affects plays that start after it; there's no way back to one-shot in Blitz. |
| `SoundPitch sound,hertz` | Sets the sample rate later plays start at | `snd.SetSampleRate(hertz)` | fbengine_audio E9 | Hz, not a ratio: the file's own rate is normal speed (11025 plays an 11025 Hz file as recorded). miniaudio wants a ratio: `hertz / snd.FileSampleRate()`. Stored as a default applied at the next PlaySound (gxSound::setDefaults), never to playing channels. |
| `SoundVolume sound,volume#` | Sets the volume later plays start at, 0 to 1 | `snd.SetVolume(volume)` | fbengine_audio E9 | Linear amplitude, `volume*255` truncated to FMOD's 0..255 (0.5 is 127/255), clamped: above 1 doesn't amplify. Default plays only. |
| `SoundPan sound,pan#` | Sets the pan later plays start at, -1 left to 1 right | `snd.SetPan(pan)` | fbengine_audio E9 | `(pan+1)*127.5` truncated: 0 becomes 127, one step left of FMOD's center 128. Pan law: see behaviors. Default plays only. |
| `PlaySound(sound)` | Plays the sound on a free voice and returns its channel | `snd.Play()`, a `Channel` | fbengine_audio E9 | Every call is a new voice: the same sound overlaps itself. `PlaySound 0` returns 0. A sound from Load3DSound played this way is spatialized at FMOD's default 3D position (the world origin, at rest), not centered; check against Blitz. |
| `PlayMusic(file$)` | Streams a file (or plays a tracker module or MIDI) and returns its channel | `Try Music.Play(path)`, a `Channel` | fbengine_audio E9 | The help says it reloads each time; the source caches by lowercased path (gxAudio::playFile): a second PlayMusic of the same file restarts that one stream and returns the same channel, and it's never freed. Format is chosen by a substring of the path: ".raw", ".wav", ".mp2", ".mp3", ".ogg", ".wma", ".asf" anywhere in it stream (FSOUND_Stream), anything else goes to FMUSIC (mod, s3m, xm, it, mid, rmi). Trackers, MIDI, and WMA/ASF: design question 6. On a module, ChannelPitch and ChannelPan do nothing and ChannelVolume sets its master volume. |
| `PlayCDTrack(track[,mode])` | Plays an audio CD track: mode 1 once, 2 loop, 3 to the CD's end | `Try Music.Play("res://music/track{track}.ogg")`, looped for mode 2 | dropped | No CD audio on modern machines (SDL3 has none). The migrator emits a Music.Play of a ripped file and a TODO; mode 3 becomes a playlist TODO. On a CD channel ChannelPlaying is always True and pitch and pan do nothing. |
| `Load3DSound(file$)` | Loads a sample for 3D playback (EmitSound) | `Try Sound.Load(path, SoundOptions(spatial: True))` | fbengine_audio E9 | Forced to mono at load (FSOUND_FORCEMONO): stereo files are downmixed. Blitz3D-only (#ifdef PRO). |

## Channels

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `StopChannel channel` | Stops the channel | `ch.Stop()` | fbengine_audio E9 | Works on sound, stream, module, and CD channels. Games call it on finished channels all the time: a finished or stopped channel's methods must do nothing, not fail (design question 5). `StopChannel 0` is a no-op. A stopped music stream can be restarted by PlayMusic of the same file. |
| `PauseChannel channel` | Pauses it | `ch.Pause()` | fbengine_audio E9 | |
| `ResumeChannel channel` | Resumes a paused channel | `ch.Resume()` | fbengine_audio E9 | |
| `ChannelPitch channel,hertz` | Sets a playing channel's sample rate in Hz | `ch.SetSampleRate(hertz)` | fbengine_audio E9 | Hz as SoundPitch (FSOUND_SetFrequency). On a 3D channel Doppler multiplies on top. |
| `ChannelVolume channel,volume#` | Sets a channel's volume, 0 to 1 | `ch.SetVolume(volume)` | fbengine_audio E9 | Same scaling and clamping as SoundVolume; on a 3D channel, distance attenuation multiplies on top. |
| `ChannelPan channel,pan#` | Sets a channel's pan, -1 to 1 | `ch.SetPan(pan)` | fbengine_audio E9 | As SoundPan. On a 3D channel FMOD's 3D pan overrides it. |
| `ChannelPlaying(channel)` | 1 while it plays, 0 once it ended or stopped | `ch.Playing()` | fbengine_audio E9 | 0 for channel 0. FMOD 3's FSOUND_IsPlaying most likely counts a paused channel as playing; check. CD channels always 1. |

## 3D sound (bbblitz3d.txt's, listed here for the audio design; counted in the 3D group's map)

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `CreateListener(parent[,rolloff_factor#][,doppler_scale#][,distance_scale#])` | Makes the one listener entity, a child of parent (a camera) | `Listener.Attach(cam, ListenerOptions(rolloff: 1, doppler: 1, distanceScale: 1))` | fbengine_audio E9 | Only one: a second is "Listener already created" in debug. Its position, forward (the entity's world Z axis), and up (world Y) go to FMOD at each RenderWorld, not at UpdateWorld. Freeing it (or its parent) puts FMOD's listener back at the origin. Code that moves the listener handle itself gets a Pivot child to attach to. |
| `EmitSound(sound,entity)` | Plays a 3D sound at an entity, following it, and returns the channel | `entity.EmitSound(snd)`, through fbengine_audio's `Extend Entity` | fbengine_audio E9 | Positions are the entity's world position (not render-tweened), updated each RenderWorld while the channel plays. Freeing the entity leaves its sounds playing where it was (velocity zeroed). A sound from LoadSound (2D) plays unspatialized. "No Listener created" in debug without one. |

## TCP and UDP

std.net (`Import std.net`): `UdpSocket`, `Datagram`, `TcpStream`, `TcpListener`, `Dns`,
`IpAddress`, `SocketAddress`, `Lan`. Blitz's sockets are IPv4 only, IPs are Int32s in host byte
order, and a socket is a Blitz stream that bbstream's ReadInt, WriteLine, ReadAvail, Eof, and the
rest work on. The migrator retypes IP variables as `IpAddress?` (0 becomes Null), and wraps each
socket in a small compat stream (`BlitzTcpStream`, `BlitzUdpStream`, E13) holding std's socket
and a `BinaryReader`/`BinaryWriter` in `ByteOrder.LITTLE`, so bbstream's commands translate one
to one; modern code uses std's sockets directly.

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `DottedIP$(ip)` | An Int IP as "a.b.c.d" | `ip.ToString()` | std | Most significant byte first: the Int is in host order (bbDottedIP). Compat: `BlitzIp.ToAddress(n)` builds `IpAddress.V4` from its four bytes. |
| `CountHostIPs(host$)` | Looks up a host's IPv4 addresses, keeps them for HostIP, and returns how many | `Try Dns.Resolve(host)`, its `IsV4` ones; for "" `Lan.LocalAddresses()`, its `IsV4` ones | std | gethostbyname: IPv4 only, 0 on failure (std throws NetError HOST_NOT_FOUND). The common idiom `CountHostIPs("")` asks for this machine's own addresses; `Dns.Resolve("")` throws, so the migrator maps "" to Lan.LocalAddresses (no loopback, as gethostbyname("") had none). |
| `HostIP(host_index)` | The index-th address of the last CountHostIPs, from 1 | `addresses[index - 1]` | std | 1-based. Out of range: "Host index out of range" in debug, undefined in release. |
| `CreateUDPStream([port])` | Binds a UDP socket to a port on every interface, 0 for any port | `Try UdpSocket.Bind(SocketAddress(IpAddress.Any, port))` | std | 0 on failure (port in use) in Blitz; std throws NetError. |
| `CloseUDPStream udp_stream` | Closes it | `udp.Close()` | std | |
| `SendUDPMsg udp_stream,dest_ip[,dest_port]` | Sends what was written to the stream since the last send as one datagram | `Try udp.Send(written.ToArray(), SocketAddress(ip, port))` | std | Writes (WriteInt etc.) go into a buffer that SendUDPMsg sends and empties; a port of 0 sends to the socket's own port (symmetric peers rely on it). A failed send leaves the buffer and puts the stream into an error state for good (later reads and writes do nothing). |
| `RecvUDPMsg(udp_stream)` | Takes the next datagram into the stream's read buffer and returns the sender's IP, or 0 | `Try udp.Receive(timeout)`, catching TimeoutError as none | std | Waits UDPTimeouts' ms, 0 by default, which doesn't wait (std: a 0 Duration doesn't wait either). Unread bytes of the previous datagram are dropped. Windows' ICMP "port unreachable" errors are skipped (the loop retries). The datagram's bytes then feed bbstream's reads. |
| `UDPStreamIP(udp_stream)` | The socket's own IP | `udp.LocalAddress.Address` | std | getsockname of a socket bound to every interface: 0 (0.0.0.0), not the machine's address. |
| `UDPStreamPort(udp_stream)` | The socket's own port | `udp.LocalAddress.Port` | std | The OS's pick when created with 0. |
| `UDPMsgIP(udp_stream)` | The last received datagram's sender IP | `datagram.From.Address` | std | Before any receive, the socket's own address (in_addr starts as addr). |
| `UDPMsgPort(udp_stream)` | The last received datagram's sender port | `datagram.From.Port` | std | |
| `UDPTimeouts recv_timeout` | Sets how long RecvUDPMsg waits, in ms, for every UDP stream | the `timeout` of each `Receive`, `recv.Milliseconds` | std | Global in Blitz; the compat stream keeps one value. |
| `OpenTCPStream(server$,server_port[,local_port])` | Connects to a host (name or dotted IP) and returns the stream, or 0 | `Try TcpStream.Connect(server, port)` | std | Blitz blocks with the OS's connect timeout and tries only the first IPv4 address; std tries each address Dns.Resolve gives, IPv6 too. `local_port` (bind before connecting) has no std equivalent: dropped with a migrator warning (it's almost never used). |
| `CloseTCPStream tcp_stream` | Closes it | `stream.Close()` | std | Blitz sets SO_DONTLINGER only when the stream had an error. |
| `CreateTCPServer(port)` | Listens on a port on every interface, or 0 | `Try TcpListener.Listen(SocketAddress(IpAddress.Any, port))` | std | Backlog SOMAXCONN. |
| `CloseTCPServer tcp_server` | Closes the server and every stream it accepted | `listener.Close()`, and each accepted stream's `Close()` | std | std's Close closes only connections nothing accepted; Blitz also closes the accepted ones (TCPServer's destructor), so the compat layer tracks them. Those handles then dangle in Blitz (still in tcp_set): a remake can treat them as closed. |
| `AcceptTCPStream(tcp_server)` | The next waiting connection, or 0 | `Try listener.Accept(timeout)`, catching TimeoutError as none | std | Waits TCPTimeouts' accept ms, 0 by default: it doesn't block. Also pumps window messages (and ends the program if the window closed). |
| `TCPStreamIP(tcp_stream)` | The other side's IP | `stream.RemoteAddress.Address` | std | 0 if getpeername failed. |
| `TCPStreamPort(tcp_stream)` | The other side's port | `stream.RemoteAddress.Port` | std | |
| `TCPTimeouts read_millis,accept_millis` | Sets how long TCP reads wait for their bytes and how long AcceptTCPStream waits | `stream.ReadTimeout = read.Milliseconds`, and Accept's `timeout` | std | Defaults: read 10000 ms, accept 0. A Blitz read waits for all the bytes it asked for (ReadInt's 4) for up to the timeout; when it times out the stream goes into an error state for good and Eof returns -1. A read timeout of 0 makes any read that finds no data kill the stream. |

Stream behaviors on sockets that bbstream's map must agree with: TCP `Eof` is 1 after the other
side closed gracefully, -1 after an error; TCP `ReadAvail` is FIONREAD's count, which std's
TcpStream has no call for (design question 8); a TCP write is one `send()`, which can be partial,
with no retry. UDP `Eof` is True once the received datagram is read through.

## DirectPlay (multiplay)

DirectPlay 4 (DX7) is gone: Windows ships it only as an optional legacy feature, it's
Windows-only, and its protocol (TCP 47624 plus 2300-2400 for sessions, lobbies, host migration)
has no maintained implementation. Blitz uses it as peer-to-peer sessions with
DPSESSION_KEEPALIVE, MIGRATEHOST, OPTIMIZELATENCY, and the DirectPlay protocol.

std.net.game is what replaces it: `GameHost` (one end of any number of connections over UDP,
`GameHost.Bind`), `GamePeer` (a connection, `Send(channel, bytes or text)`, `State`,
`RoundTripTime`, `PacketLoss`), `GameEvent`s (`CONNECT`, `RECEIVE`, `DISCONNECT` with a
`DisconnectReason`), and `GameSettings.Channels` of `ChannelKind` (`RELIABLE_ORDERED`,
`RELIABLE_UNORDERED`, `UNRELIABLE`), with fragmentation, acks, and resends; std.net's
`LanAnnouncer` and `Lan.Find` find games on the LAN by mDNS. It's client-server (a host and its
peers), not DirectPlay's every-player-to-every-player mesh, and it has no host migration.

The migrator emits a `NetGame` compat module (E13, about 300 lines over std.net.game) that keeps
Blitz's model: the host relays, so every player still "sees" every other; message types and
player ids travel in each message's first bytes; channel 0 is RELIABLE_ORDERED for `reliable=1`
and channel 1 UNRELIABLE for `reliable=0`; game names go out through `LanAnnouncer` (type
`"_blitznet._udp"`, the game's name as the service name). A port of a game that a human
modernizes uses GameHost directly.

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `StartNetGame()` | Shows DirectPlay's Windows dialog to host or join; 0 failed, 1 joined, 2 hosting | `NetGame.Start()` | fbengine E13 | A picker drawn with fbengine_ui (E11) listing `Lan.Find` results and a Host button (design question 9). Modem and serial connections are dropped. "Multiplayer game already started" if one is. |
| `HostNetGame(game_name$)` | Hosts a game with that name; 2 on success, else 0 | `NetGame.Host(name)` | fbengine E13 | `GameHost.Bind(SocketAddress(IpAddress.Any, port))` plus a `LanAnnouncer`. Returns 2, as Blitz. |
| `JoinNetGame(game_name$,ip_address$)` | Joins the game of exactly that name at that address; 1 on success, else 0 | `NetGame.Join(name, address)` | fbengine E13 | Blitz enumerates the sessions at the IP and opens the first whose name matches exactly (case-sensitive); an empty address broadcasts on the LAN. The shim connects with `GameHost.Connect`, then checks the name in the host's welcome; "" uses `Lan.Find`. Synchronous: wait (Service) until CONNECTED or DISCONNECT. |
| `StopNetGame` | Leaves or ends the game | `NetGame.Stop()` | fbengine E13 | Clears every player. Safe to call when none is started. |
| `CreateNetPlayer(name$)` | Makes a local player; returns its id, or 0 | `NetGame.CreatePlayer(name)` | fbengine E13 | A player is needed before sending or receiving. Several local players per machine are allowed. Ids are DirectPlay DPIDs: nonzero 32-bit values; the shim gives the host's ids. Other machines get message 100 for it. |
| `DeleteNetPlayer player` | Deletes a local player | `NetGame.DeletePlayer(id)` | fbengine E13 | Others get message 101. |
| `NetPlayerName$(player)` | A player's name | `NetGame.PlayerName(id)` | fbengine E13 | "<all>" for 0, "<unknown>" for an id it doesn't know. |
| `NetPlayerLocal(player)` | 1 if the player was created on this machine | `NetGame.PlayerLocal(id)` | fbengine E13 | 0 for unknown ids. |
| `SendNetMsg(type,data$,from_player[,to_player][,reliable])` | Sends a message from a local player to one player or (0) all | `NetGame.Send(type, data, from, to, reliable)` | fbengine E13 | `type` is sent as one byte (a char): 1-99 by the help; anything past 255 wraps. `data$` is binary-safe (its length is sent). Defaults to=0 (all), reliable=1. Returns 1 if DirectPlay took it. The from id is the sender's word, unchecked (bbMsg header). A machine never receives its own players' broadcasts (filtered on receive). |
| `RecvNetMsg()` | Takes the next message into NetMsgType/From/To/Data; 1 if there was one | `NetGame.Receive()` | fbengine E13 | Non-blocking. Order: first a 100 for each remote player not yet reported (players already in the game at join, then new ones), then DirectPlay's queue. System messages: 100 player joined (From is the player), 101 player left, 102 this machine became the host (migration), 200 session lost. With no host migration in std.net.game, the shim never gives 102 and gives 200 when the host goes. |
| `NetMsgType()` | The last message's type | `NetGame.MessageType()` | fbengine E13 | 0 before any. |
| `NetMsgFrom()` | The last message's sender id | `NetGame.MessageFrom()` | fbengine E13 | For 100 and 101, the player who joined or left. DPID_UNKNOWN when none. |
| `NetMsgTo()` | The last message's recipient id (0 for all) | `NetGame.MessageTo()` | fbengine E13 | DPID_ALLPLAYERS (0) for broadcasts and system messages. |
| `NetMsgData$()` | The last message's data | `NetGame.MessageData()` | fbengine E13 | "" for system messages. |

## Behaviors to match exactly

1. Every input read pumps the window's messages when DirectInput isn't in use, so a loop like
   `While Not KeyHit(1) : Wend`, with no Flip, still sees the key (gxruntime/gxinput.cpp:
   Keyboard::update and Mouse::update call `runtime->idle()`; gxruntime.cpp: gxRuntime::idle).
   fbengine reads events only at Flip, so that loop would hang: see design question 3.
2. While the game is suspended (fullscreen, or windowed with auto-suspend, after losing focus),
   idle() blocks in GetMessage, so any input read freezes the game until it's back
   (gxRuntime::idle).
3. KeyHit, MouseHit, and JoyHit return a press count and zero it on read; Flush* clears counts
   and the queue but not held state (gxruntime/gxdevice.cpp: keyHit, flush).
4. Auto-repeat is ignored: WM_KEYDOWN with the previous-state bit set is dropped
   (gxruntime.cpp: windowProc, `lparam & 0x40000000`).
5. A key code is `((lparam>>17)&0x80)|((lparam>>16)&0x7f)` in windowed mode, DIK's offset under
   DirectInput (windowProc; Keyboard::update). Pause and Num Lock most likely swap in windowed
   mode, and Print Screen never goes down there (no WM_KEYDOWN for it); verify both on Windows.
6. Suspend and resume reset every device: keys up, counts zero, mouse x, y, and wheel back to 0;
   the MouseXSpeed baselines (statics in bbinput.cpp) aren't reset, so the first speed after a
   resume jumps (gxDevice::reset via gxInput::reset, gxRuntime::acquireInput/unacquireInput).
7. In windowed mode a key released while another window has focus is never seen, so it stays
   down (no WM_KEYUP arrives). That's a Blitz bug: SDL3 resets the keyboard on focus loss, and
   fbengine shouldn't copy the bug.
8. GetKey's ASCII ignores Caps Lock and Num Lock and maps the ten navigation keys to 1-6 and
   28-31 (gxInput::toAscii); the key queue holds 32 presses and drops the rest
   (gxDevice::downEvent, QUE_SIZE).
9. MouseX/Y are clamped to the graphics size and scaled in scaled windowed mode; MouseZ is the
   accumulated wheel delta / 120, truncated (windowProc WM_MOUSEMOVE and WM_MOUSEWHEEL;
   bbMouseZ).
10. MouseXSpeed is since the last call, and MoveMouse resets its baseline (bbMouseXSpeed,
    bbMoveMouse).
11. Joysticks: enumerated once, ports 0-based, out-of-range ports return 0; axes normalized with
    no dead zone, absent axes -1; Pitch/Yaw/Roll times 180; hat in whole degrees, -1 centered;
    Dir thresholds at strict ±1/3; buttons 1-31 (gxinput.cpp: Joystick::update; bbinput.cpp).
12. SoundPitch, SoundVolume, and SoundPan set defaults applied at the next play only
    (gxSound::setPitch and setDefaults); LoopSound changes the sample's mode at once
    (gxSound::setLoop).
13. Volume is `volume*255` and pan `(pan+1)*127.5`, both truncated to FMOD's 0..255, so pan 0 is
    127 (gxaudio.cpp: SoundChannel::setVolume and setPan; gxSound::setPan). Pitch is a sample
    rate in Hz (FSOUND_SetFrequency).
14. Pan law: whatever FMOD 3.75's software mixer does at 0..255 (linear or constant power)
    decides how loud a centered mono sound is on each side. Measure it with Blitz once and set
    miniaudio's pan (ma_pan_mode) to match.
15. Channel objects are reused round-robin once they stop, after the first 4096 plays
    (gxaudio.cpp: allocSoundChannel), so in Blitz a very stale handle can control a newer
    sound; operations on a finished channel otherwise do nothing visible.
16. PlayMusic caches by lowercased path and restarts the same stream on a second call; streams
    are chosen by a substring match on the extension (gxAudio::playFile).
17. Audio keeps playing when the window loses focus (FSOUND_INIT_GLOBALFOCUS in
    gxRuntime::openAudio; gxAudio::pause is empty).
18. 3D sound: Load3DSound forces mono (gxAudio::loadSound). Blitz never sets min or max distance,
    so FMOD's defaults hold (1 and 1e9): gain is `1 / (1 + rolloff * (d - 1))` past 1 unit, 1
    inside it, which is miniaudio's inverse model with min 1 and max 1e9 (FSOUND_3D_SetRolloffFactor
    in gxAudio::set3dOptions). FMOD 3's software 3D is attenuation, stereo pan by direction, and
    Doppler: no HRTF.
19. 3D positions are left-handed (Blitz and FMOD 3 both); miniaudio is right-handed with -Z
    forward, so z is negated for positions, velocities, and the listener's forward and up
    (listener.cpp: renderListener passes world Z as forward, world Y as up).
20. An emitter's and listener's velocity is the world position's change over the last
    UpdateWorld divided by its anim_speed, so in units per UpdateWorld, not per second
    (object.cpp: Object::endUpdate); Doppler uses it as FMOD's units per second, so Blitz's
    Doppler is far weaker than physical. Feed miniaudio the same per-update velocity, and match
    FMOD 3's speed of sound and distance factor (FSOUND_3D_SetDistanceFactor), which scales units
    to meters for Doppler; whether it changes attenuation too needs a measurement.
21. 3D positions and the listener update in RenderWorld, not UpdateWorld or Flip (World::render
    calls renderListener; Object::beginRender calls updateSounds): a game that doesn't render
    doesn't move its sounds. Emitters use the world transform, not the tweened one.
22. Freeing an entity leaves its emitted sounds playing at its last position, at rest
    (Object::~Object). Freeing the listener resets FMOD's listener to the origin
    (Listener::~Listener).
23. UDP: writes buffer until SendUDPMsg; port 0 sends to the socket's own port; RecvUDPMsg waits
    UDPTimeouts' ms (0: doesn't) and returns the sender's IP; UDPStreamIP is 0 (bbsockets.cpp:
    UDPStream::send, recv, getIP).
24. TCP: reads wait for every byte asked, up to the read timeout (10 s by default), and a timeout
    or error puts the stream into an error state for good; AcceptTCPStream doesn't block by
    default; CloseTCPServer closes its accepted streams (bbsockets.cpp: TCPStream::read,
    TCPServer::accept and ~TCPServer, sockets_create).
25. IPs are Int32 in host byte order everywhere; `CountHostIPs("")` gives this machine's IPv4
    addresses; HostIP is 1-based (bbCountHostIPs, bbHostIP).
26. DirectPlay messages: type is one byte; a machine skips its own local players' messages;
    remote players are announced as 100 before any queued message; 101, 102, and 200 as listed
    (multiplay.cpp: bbRecvNetMsg, bbSendNetMsg, Player).

## Design questions

1. **Where the migrator's compat shims live.** GetKey/WaitKey/GetMouse/GetJoy/JoyHat/JoyXDir's
   ASCII and DirectInput numbering, the socket streams, and NetGame have no clean modern API.
   Options: in fbengine (a `fbengine.blitz` module), a separate `fbengine_compat` package, or
   source the migrator writes into each translated project. Recommendation: a small module the
   migrator writes into the project, so fbengine's API stays modern and a human can delete or
   modernize it file by file; NetGame alone is big enough to be its own file.
2. **Key's values.** Recommendation: `Enum Key: Int32` with Blitz's scancodes as the values
   (`ESCAPE = 1`, `UP = 200`), and new keys from 256, so `KeyDown(k)` over a computed int and
   saved key bindings migrate as `k As Key`, and the native layer maps SDL3's scancodes to these.
   The alternative, SDL's own numbers, makes every computed scancode need a lookup table.
3. **Input read outside Flip.** Blitz games poll input in loops with no Flip (menus waiting for a
   key, `While Not KeyHit(1) : Wend`), which work only because each read pumps messages.
   Recommendation: add `PollInput()` to E1 step 3, which reads the OS's events into the input
   state as Flip does, without presenting or stepping the scheduler, and `WaitInput()`, which
   blocks until a press; the migrator inserts PollInput in loops that read input and don't
   Flip. Also add `Input.Presses()`, the ordered presses the last Flip or PollInput read, for
   the Get* queues and for E1 step 6's "rebinding from the next input pressed", and
   `KeyChar(key, shift)` for GetKey's ASCII (SDL_GetKeyFromScancode without Caps Lock).
4. **Raw joysticks.** SDL3's gamepad API covers mapped controllers only. Flight sticks, wheels,
   and Blitz's U, V, and Roll axes need SDL3's joystick API. Recommendation: add `Joysticks()` to
   E1 step 5, each with Axis(i), Button(i), and Hat(i), unmapped and with no dead zone; the
   compat layer uses a gamepad when SDL maps the device and the raw joystick otherwise. Also add
   `RawAxis` (before the dead zone) to Gamepad, since Blitz's axes have none.
5. **Finished channels.** engine.md says a freed handle is a clear error. Blitz games call
   StopChannel, ChannelVolume, and ChannelPlaying on channels that finished on their own, every
   frame. Recommendation: `Channel` is a generational value whose methods do nothing (and
   Playing() is False) once its voice ended or was stopped; only a Sound used after Free() is an
   error. The 4096-play aliasing isn't reproduced.
6. **Trackers, MIDI, WMA/ASF, MP2, RAW.** miniaudio plays none of them. Recommendation: the mesh
   converter's sibling in fbengine_tools converts them to OGG once, at migration (libopenmpt or
   libxmp for mod/s3m/xm/it, a General MIDI SoundFont renderer for .mid/.rmi, Media Foundation
   or FFmpeg for wma/asf), and the migrator rewrites the paths; runtime support for trackers can
   come later as an fbengine_audio decoder if a ported game needs pattern-accurate looping.
7. **Sound loading and the asset cache.** fbengine_audio is its own package, so E8's
   `Assets.Load<T>` can't name `Sound` unless the cache is generic over a loader a package
   registers. Recommendation: `Sound.Load(path, options)` in E9, uncached as Blitz's is, and a
   loader fbengine_audio registers with E8's cache so `Assets.Load<Sound>` works too.
8. **ReadAvail on a TcpStream.** Blitz programs poll `ReadAvail(stream)` to avoid blocking; std's
   TcpStream has no call for the bytes waiting. Options: new in std, `TcpStream.Available()`
   (FIONREAD through libuv, which doesn't expose it directly), or the compat stream reading with
   a zero ReadTimeout into its own buffer. Recommendation: the compat stream's buffer, with no
   change to std.
9. **StartNetGame's dialog.** Recommendation: the NetGame shim draws a minimal host/join picker
   with fbengine_ui (E11) over `Lan.Find`, modem and serial dropped; a game that already uses
   HostNetGame/JoinNetGame needs no UI at all.
10. **Audio updates each frame.** 3D emitters and the listener must follow their entities, but
    fbengine can't call fbengine_audio. Recommendation: fbengine_audio runs a coroutine on
    `Scheduler.Main` (`Await NextTick()` in a loop), which Flip steps; it updates positions and
    velocities from each entity's world transform. That moves sound updates from Blitz's
    RenderWorld to Flip, the same frame for any game that renders; note it in the migration
    guide.
11. **Mouse coordinates and high DPI.** Recommendation: MouseX/Y in Graphics' width and height
    units (what 2D drawing uses), clamped as Blitz's, whatever the window's pixel scale or size.

## Counts

| Where | Commands |
|---|---|
| fbengine E1 | 24 (KeyDown, KeyHit, FlushKeys, MouseDown, MouseHit, MouseX, MouseY, MouseZ, MouseXSpeed, MouseYSpeed, MouseZSpeed, FlushMouse, MoveMouse, JoyDown, JoyHit, JoyX, JoyY, JoyZ, JoyU, JoyV, JoyPitch, JoyYaw, JoyRoll, FlushJoy) |
| fbengine_audio E9 | 16 (every bbaudio command but PlayCDTrack) |
| fbengine E13 (compat shims) | 29 (GetKey, WaitKey, GetMouse, WaitMouse, MouseWait, JoyType, GetJoy, WaitJoy, JoyWait, JoyHat, the five Joy*Dir; the 14 multiplay commands) |
| std | 20 (every bbsockets command) |
| dropped | 3 (EnableDirectInput, DirectInputEnabled, PlayCDTrack) |
| **Total** | **92** (41 input, 17 audio, 20 sockets, 14 multiplay) |

Also mapped but counted in the 3D group's file: CreateListener and EmitSound (bbblitz3d.txt).

Skipped as internal: none. These four lists have no `_` names. The runtime's own hooks
(input_create, input_destroy, audio_create, sockets_create, multiplay_create, the *_link
functions) aren't registered commands.
