# Blitz3D to FutureBlitz: core (runtime, math, strings, files, streams, banks)

Group "core": every command in basic.txt, bbbank.txt, bbfilesystem.txt, bbmath.txt,
bbruntime.txt, bbstream.txt, bbstring.txt, and userlibs.txt. Sources read: SCRATCH/blitz3d's
bbruntime/{basic,bbbank,bbfilesystem,bbmath,bbruntime,bbstream,bbstring,userlibs}.cpp,
gxruntime/{gxruntime,gxfilesystem,gxdir,gxtimer}.cpp, stdutil/stdutil.cpp (itoa, ftoa),
compiler/{exprnode,varnode,declnode}.cpp and codegen_x86 (casts, operators, arrays),
bbruntime_dll/bbruntime_dll.cpp (FPU mode, release error box), and the 2D help pages; on
FutureBlitz's side docs/spec/02, 03, 09, 12, 14, docs/stdlib.md, std/{core,math,text,io,os,time,
ffi,log,debug,collections}, and fbengine's window.fb and frame.fb.

Almost none of this is fbengine's: it's the language's and std's. What std can't say in one exact
call goes to a small **compat module** the migrator's output imports. In the tables:

- **Where** `std`, `language`, `fbengine E<n>`, and `dropped` are as the brief says.
- **Where** `compat (E13)` is the migrator's runtime module, built with the migrator in E13: a
  `Blitz` Struct of Static Methods (`Blitz.Mid(s, 2, 3)`) and a few Classes (`BlitzBank`,
  `BlitzStream`, `BlitzDir`, `BlitzTimer`, `BlitzList<T>`, `BlitzObject`, `BlitzData`), in
  FutureBlitz over std. The `Blitz.` prefix keeps it apart from std's names (std.math's
  `Rand()` is a Float with no arguments; Blitz's is an Int from two) and gives the AI pass
  something to grep for when it modernizes. Where to put it is design question 1.

Strings: FutureBlitz's are UTF-8; Blitz's were bytes, which on a western Windows are
Windows-1252 text. The tables assume design question 2's answer: the migrator converts .bb source
from Windows-1252 to UTF-8, files' text is decoded and encoded as Windows-1252, and compat's
string positions count characters (code points), so each Blitz byte is one FutureBlitz character.

---

## Program and system (bbruntime.cpp)

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `End` | Ends the program: unwinds the runtime, closes every open file, frees everything | `Process.Exit(0)` | std | Process.Exit runs no Defer or Destroy. Blitz closed its files on End (`filesystem_destroy`), so compat's streams must hold no unwritten bytes (FileStream holds none; a compat BufferedWriter would need flushing first). At top level after the last line the migrator emits nothing. |
| `Stop` | Halts in the IDE's debugger; a no-op without one (`DummyDebugger::debugStop`) | `Breakpoint()` | std | std.debug's Breakpoint does nothing without a debugger, as Blitz's release runtime did. |
| `AppTitle title$[,close_prompt$]` | Sets the window's title, and an OK/Cancel box shown when the close button is pressed | `Window.SetTitle(title)`, and `Window.SetClosePrompt(prompt)` | fbengine E1 (SetTitle exists; SetClosePrompt new) | Can come before `Graphics` (SetTitle already allows it). Without a prompt, Blitz's close button ends the program at the next `idle()` (any Flip, Delay, WaitTimer, input poll): design question 4. With one, Cancel keeps it running; the box's title is the app title. |
| `RuntimeError message$` | Ends the program with the message; release builds show it in a "Error!" message box | `Blitz.RuntimeError(message)`: fbengine's message box, then `Process.Exit(1)` | compat (E13), over fbengine E1 (new `ShowMessage`) | Message cut to 255 bytes (`bbRuntimeError`). `Panic(message)` is the language's, but a player who double-clicked the game sees nothing on stderr: design question 5. |
| `ExecFile command$` | ShellExecutes a file, document, or URL, with what follows the first word (or a quoted name) as its parameters | `Blitz.ExecFile(command)`: `Try? OpenDefault(target)` with no parameters, else `Try? Command(program).Args(parts).Spawn()` | compat (E13), over std | Splits as `gxRuntime::execute` does: leading spaces skipped, a name in quotes or up to the first space, the rest trimmed as one parameter string. Doesn't wait; failure is silent. Windows' programs (calc.exe) won't exist elsewhere. |
| `Delay millisecs` | Sleeps, pumping the window's messages every 100 ms at most; ends the program if the window was closed | `Sleep((Max(ms, 0) As Long).Milliseconds)` | std | std.time's Sleep pumps nothing, so a long Delay with a window open stops its events (the OS calls it "not responding" after about 5 s) and doesn't end on close: design question 4. Negative ms returns at once in Blitz. |
| `MilliSecs()` | Milliseconds from `timeGetTime()`, since Windows started, wrapping as an Int | `Blitz.MilliSecs()`: `(Instant.Now().Nanoseconds / 1_000_000) As Int` | compat (E13), over std | Only differences mean anything; `As Int` from Int64 keeps the low bits, so it wraps as timeGetTime does (and goes negative, as Blitz's did after 24.8 days' uptime). Blitz set `timeBeginPeriod(min)`, so 1 ms resolution. `SeedRnd MilliSecs()` works the same. |
| `CommandLine$()` | The program's parameters as one string, spaces trimmed off both ends | `Blitz.CommandLine()`: `" ".Join(Process.Arguments())` | compat (E13), over std | Process.Arguments splits as C does, so quotes inside the original line are gone: `"a b" c` comes back as `a b c`. Good enough for `/debug`-style flags. |
| `SystemProperty$(property$)` | A folder or a Win32/DX handle by name, case-insensitive; "" for anything else | `Blitz.SystemProperty(name)` | compat (E13), over std | `appdir`: the exe's folder with a trailing separator (`Path(Folders.Executable).Parent()`), not Program Files as the help says; `tempdir`: `Dir.Temp()` plus a separator; `windowsdir`/`systemdir`: `Env.Get("SystemRoot")` (and `\System32`) on Windows, "" elsewhere; `cpu`: always "Intel"; `os`: Blitz names Windows 95 to 7 and gives "" on Windows 8 and later, so compat names `System.Platform`; `apphwnd`, `apphinstance`: fbengine's native handle if it gets one (only for DLLs, dropped otherwise); `direct3d7`, `direct3ddevice7`, `directdraw7`, `directinput7`: "" (dropped, no DX7). |
| `GetEnv$(env_var$)` | The variable's value, "" when unset | `Env.Get(name) ?? ""` | std | |
| `SetEnv env_var$,value$` | `putenv("name=value")` | `Env.Set(name, value)` | std | On Windows `putenv("X=")` removes X, so a value of "" is `Env.Unset(name)`. |
| `CreateTimer(hertz)` | A periodic multimedia timer of `1000/hertz` ms, integer division | `BlitzTimer(hertz)` | compat (E13), over std's `Instant` and `Sleep` | Period is whole ms: 60 Hz ticks every 16 ms, 62.5 Hz; 70 Hz is 71.4; 30 Hz is 30.3. A remake has to keep that or the game runs ~4% slow. `hertz` above 1000 gives period 0 (timeSetEvent fails, WaitTimer never returns); 0 divides by zero. When a game only uses it to cap the frame rate, the AI pass can make it `Frame.SetLimit(hz)` (fbengine E1). |
| `WaitTimer(timer)` | Blocks until the timer has ticked, then gives how many ticks since the last WaitTimer (at least 1) | `timer.Wait()` | compat (E13) | Ticks are counted from creation (`ticks_put - ticks_get`), so a slow frame gives 2 or more, which fixed-step loops (`For k = 1 To WaitTimer(t)`) rely on. Ends the program if the window was closed. |
| `FreeTimer timer` | Stops and frees it; an unknown one is ignored | `timer.Free()` | compat (E13) | |
| `DebugLog text$` | A line in the IDE's debug log; nothing in release builds | `log.Debug(text)`, with `Global log = Logger("game")` | std | std.log's default filter is INFO, so nothing shows unless FB_LOG asks, as in Blitz's release. |

## Math (bbmath.cpp)

All of Blitz's math is Float32, with the FPU set to 24-bit precision and round-to-nearest
(`_control87(_RC_NEAR|_PC_24|...)` in bbruntime_dll.cpp), every exception masked (a float
divided by 0 is Inf, not an error). Angles are degrees; std.math's are radians, and its `Deg`
turns degrees into radians.

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `Sin(degrees#)` | sine of an angle in degrees | `Sin(Deg(a))` | std | Blitz multiplies by a Float `dtor` and takes C's double `sin`; std's Deg works in Doubles. Results can differ in the last bit. |
| `Cos(degrees#)` | cosine, degrees | `Cos(Deg(a))` | std | as Sin |
| `Tan(degrees#)` | tangent, degrees | `Tan(Deg(a))` | std | as Sin |
| `ASin(float#)` | arcsine, in degrees | `ToDegrees(Asin(x))` | std | Out of [-1, 1] is NaN in both. |
| `ACos(float#)` | arccosine, in degrees | `ToDegrees(Acos(x))` | std | |
| `ATan(float#)` | arctangent, in degrees | `ToDegrees(Atan(x))` | std | |
| `ATan2(floata#,floatb#)` | `atan2(a, b)` in degrees: a is y, b is x | `ToDegrees(Atan2(y, x))` | std | Same argument order as C. Range -180 to 180. |
| `Sqr(float#)` | square root | `Sqrt(x)` | std | Negative is NaN (Blitz prints "NaN"). |
| `Floor(float#)` | rounds down, as a Float | `Floor(x)` | std | Gives a Float in both; assigning it to an Int then goes through `Blitz.Int`. |
| `Ceil(float#)` | rounds up, as a Float | `Ceil(x)` | std | |
| `Exp(float#)` | e to the x | `Exp(x)` | std | |
| `Log(float#)` | natural log | `Log(x)` | std | |
| `Log10(float#)` | base-10 log | `Log10(x)` | std | |
| `Rnd(from#[,to#=0])` | A Float from Blitz's own generator: `rnd()*(to-from)+from` | `Blitz.Rnd(from, to)` | compat (E13) | One argument is `Rnd(x, 0)`, so 0 to x. Never gives either end exactly (`rnd()` is `(state & 65535)/65536 + 0.5/65536`), only 65536 values, and `to < from` just works backwards, where std's `RandFloat` panics. Assigned to an Int it rounds half-to-even (`y = Rnd(0, 10)` makes 0 and 10 half as likely as the rest). |
| `Rand([from,]to)` | An Int from from to to, both included; one argument is `Rand(1, to)` | `Blitz.Rand(from, to)` | compat (E13) | `int(rnd()*(to-from+1))+from`, truncating; swaps the ends when to < from, so `Rand(0)` is 0 or 1 and `Rand(-3)` is -3 to 1. Only 65536 outcomes, so `Rand(1, 100000)` can't give most values. |
| `SeedRnd seed` | Seeds the generator: `seed & 0x7FFFFFFF`, 0 becomes 1 | `Blitz.SeedRnd(seed)` | compat (E13) | Exact sequences matter: games build levels from a seed (SCP: Containment Breach's map from its seed string). `SeedRnd(-1)` and `SeedRnd($7FFFFFFF)` make the state M, which Schrage's step turns into 0 forever, so Rnd then always gives 0.5/65536: compat reproduces it by running the same code. |
| `RndSeed()` | The generator's current state, not the last seed | `Blitz.RndSeed()` | compat (E13) | So `SeedRnd RndSeed()` continues where it was. Starts at 0x1234 (`math_create`). |

## Strings (bbstring.cpp)

Positions are 1-based. `CHKPOS` and `CHKOFF` raise their runtime errors in every build, not only
debug.

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `String$(string$,repeat)` | The string repeat times | `s.Repeated(Max(n, 0))` | std | Blitz gives "" for n <= 0 (`while(n-->0)`); std's Repeated panics on a negative count. |
| `Left$(string$,count)` | The first count characters | `Blitz.Left(s, n)` | compat (E13) | count past the end gives the whole string; negative is the error "parameter must be positive". |
| `Right$(string$,count)` | The last count characters | `Blitz.Right(s, n)` | compat (E13) | As Left. |
| `Replace$(string$,from$,to$)` | Every from, left to right, not overlapping, replaced; scanning resumes after the replacement | `s.Replace(old, replacement)` | std | Same result as std's Split-and-Join for a non-empty `old`. An empty `from` hangs Blitz when `to` is "" and inserts `to` before each character otherwise; std panics, so with a `from` that isn't a non-empty literal the migrator emits `Blitz.Replace`, which returns s unchanged for "". |
| `Instr(string$,find$[,from=1])` | 1-based position of find at or after from, 0 when absent | `Blitz.Instr(s, find, from)` | compat (E13) | from <= 0 is the error "parameter must be greater than 0"; from past the end gives 0; an empty find gives from (when from <= Len + 1). std's `Find` is 0-based, gives Null, and panics for a from past the end or inside a character. |
| `Mid$(string$,start[,count=-1])` | count characters from start, 1-based; any negative count means the rest | `Blitz.Mid(s, start, count)` | compat (E13) | start <= 0 is an error; start past the end gives ""; count past the end is clamped. |
| `Upper$(string$)` | ASCII letters to upper case (C locale `toupper`) | `s.ToUpper()` | std | std maps all of Unicode ("é" to "É", "ß" to "SS"); Blitz left anything above 127 alone. Same for ASCII. If a game's text needs Blitz's exact result, `Blitz.Upper`. |
| `Lower$(string$)` | ASCII letters to lower case | `s.ToLower()` | std | as Upper |
| `Trim$(string$)` | Drops every leading and trailing byte that `isgraph` rejects: spaces, tabs, all control bytes, DEL | `s.Trim()` | std | std trims Unicode whitespace, which leaves \0, \x01 and other control bytes; Blitz's trims them too. Rarely matters; `Blitz.Trim` if it does. |
| `LSet$(string$,size)` | Exactly size characters: cut, or padded with spaces on the right | `Blitz.LSet(s, n)` | compat (E13) | Negative size is an error. `s.PadRight(n)` pads but doesn't cut. |
| `RSet$(string$,size)` | Exactly size characters: the last size, or padded with spaces on the left | `Blitz.RSet(s, n)` | compat (E13) | Keeps the right end when cutting. |
| `Chr$(ascii)` | A one-byte string of `(char)ascii` | `Blitz.Chr(n)` | compat (E13) | The low 8 bits, as a Windows-1252 character (`Chr(128)` is "€", `Chr(233)` "é"). `Chr(0)` is a one-character string holding U+0000. `"{n As Char}"` would be wrong for 128 to 159 and panic for surrogates. |
| `Asc(string$)` | The first byte, 0 to 255; -1 for "" | `Blitz.Asc(s)` | compat (E13) | The first character encoded back to Windows-1252 (one not in it gives 63, "?"). |
| `Len(string$)` | Its length in bytes | `s.CharCount` | std | Characters, by design question 2; `s.Length` (bytes) only matches for ASCII. CharCount walks the string, so compat's string functions keep an ASCII fast path. |
| `Hex$(value)` | Eight upper-case hex digits of the 32 bits, two's complement | `"{(v As UInt32):08X}"` | language | `Hex(255)` is "000000FF", `Hex(-1)` "FFFFFFFF": always 8 digits. |
| `Bin$(value)` | 32 binary digits | `"{(v As UInt32):032b}"` | language | Always 32 digits. |
| `CurrentDate$()` | `strftime("%d %b %Y")` in local time: "30 Oct 2026" | `Date.Today().Format("dd MMM yyyy")` | std | Day zero-padded; English month abbreviations in both. |
| `CurrentTime$()` | `strftime("%H:%M:%S")`, 24-hour local time | `TimeOfDay.Now().Format("HH:mm:ss")` | std | |

## Files and folders (bbfilesystem.cpp, gxfilesystem.cpp)

Blitz's file handles are Ints, 0 on failure, and every failure is silent. compat's open
Functions give a `BlitzStream?`, Null where Blitz gave 0, and the folder commands use `Try?`.
Every path goes through `Blitz.Path(p)` if design question 3 says so (backslashes, case).

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `OpenFile(filename$)` | Opens an existing file to read and write, binary, at its start; 0 if it isn't there | `Blitz.OpenFile(path)`: `Try? File.Open(path, OpenOptions(Read: True, Write: True))` | compat (E13), over std | `filebuf::open(in\|out)` neither creates nor truncates. |
| `ReadFile(filename$)` | Opens a file to read; 0 if missing | `Blitz.ReadFile(path)`: `Try? File.Open(path)` | compat (E13), over std | |
| `WriteFile(filename$)` | Creates or truncates a file to write | `Blitz.WriteFile(path)`: `Try? File.Create(path)` | compat (E13), over std | 0 when its folder is missing. |
| `CloseFile file` | Closes it | `file.Close()` | compat (E13) | Closing an unknown handle is a debug error in Blitz; compat ignores a second Close. |
| `FilePos(file)` | The position in bytes | `file.Position()` | compat (E13), over std's `Seeker.Position()` | An Int, 32-bit, as Blitz's. |
| `SeekFile(file,pos)` | Moves to pos from the start, giving the new position | `file.Seek(pos)` | compat (E13), over `Seek(pos, SeekOrigin.START)` | Past the end is allowed, as std's Seeker allows. |
| `ReadDir(dirname$)` | Opens a folder listing (`FindFirstFile(dir\*)`); 0 if it can't | `Blitz.ReadDir(path)`: a `BlitzDir?` over `Dir.Entries(path)` | compat (E13), over std | |
| `CloseDir dir` | Closes it | `dir.Close()` | compat (E13) | Nothing to free; kept so translated code reads the same. |
| `NextFile$(dir)` | The next name, "." and ".." included; "" when done | `dir.NextFile()` | compat (E13) | Names only, no path. "." and ".." come first, as NTFS lists them; the rest in `Dir.Entries`' byte order, where NTFS's was case-insensitive, so give them case-insensitively sorted to match. Loops of `Until file$ = ""` that skip "." and ".." keep working. |
| `CurrentDir$()` | The current folder, with a trailing backslash | `Blitz.CurrentDir()`: `"{Try Dir.Current()}"` plus a separator | compat (E13), over std | Code builds paths with `CurrentDir() + "file"`, so the separator matters. |
| `ChangeDir dir$` | Sets the current folder | `Try? Dir.SetCurrent(dir)` | std | |
| `CreateDir dir$` | Makes one folder, its parent must exist | `Try? Dir.Create(dir)` | std | An existing folder fails silently in both. |
| `DeleteDir dir$` | Removes an empty folder | `Try? Dir.Remove(dir)` | std | Not empty: fails silently (std throws NOT_EMPTY, dropped by Try?). |
| `FileSize(file$)` | Size in bytes, 0 for a folder or a missing file | `((Try? File.Size(path)) ?? 0) As Int` | std | Blitz gives the low 32 bits (`nFileSizeLow`), which `As Int` keeps. FindFirstFile matches wildcards, so `FileSize("*.sav")` gave the first match's size; don't reproduce. |
| `FileType(file$)` | 0 none, 1 file, 2 folder | `File.Exists(p) ? 1 : (Dir.Exists(p) ? 2 : 0)` | std | The migrator turns `FileType(p) = 1` into `File.Exists(p)` and `= 2` into `Dir.Exists(p)` where it can. |
| `CopyFile file$,to$` | Copies, overwriting | `Try? File.Copy(from, to)` | std | |
| `DeleteFile file$` | Deletes a file | `Try? File.Delete(path)` | std | |

## Streams (bbstream.cpp)

Every read and write works on any stream: a file or a TCP stream (bbsockets, another group), so
compat's `BlitzStream` wraps any std.io Reader, Writer, and Seeker, and the TCP group's streams
are BlitzStreams too. All numbers are little-endian, as std's `BinaryReader` and `BinaryWriter`
default to; compat doesn't use them for reads because they throw at the end where Blitz gives 0.

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `Eof(stream)` | 1 at the end of a file (`sgetc() == EOF`), or when a TCP stream closed nicely; -1 on a TCP error | `stream.Eof()` | compat (E13) | An Int, since TCP gives -1 too. A file opened with WriteFile always reports 1 (its filebuf can't read), which CopyStream trips on (below). |
| `ReadAvail(stream)` | Bytes available without blocking: a file's filebuf buffer, a TCP stream's arrived bytes | `stream.ReadAvail()` | compat (E13) | For files compat gives the bytes left (size minus position); Blitz's was whatever filebuf had buffered, often 0, so no file code can rely on it. |
| `ReadByte(stream)` | One byte, 0 to 255 | `stream.ReadByte()` | compat (E13) | Unsigned. At the end: 0, no error. |
| `ReadShort(stream)` | Two bytes, unsigned, 0 to 65535 | `stream.ReadShort()` | compat (E13) | Unsigned (`int n=0` filled from the low end). A short read at the end gives the bytes there were and zeros. |
| `ReadInt(stream)` | Four bytes, signed | `stream.ReadInt()` | compat (E13) | Partial reads as ReadShort. |
| `ReadFloat#(stream)` | Four bytes of an IEEE single | `stream.ReadFloat()` | compat (E13), with `Float32.FromBits` | 0.0 at the end. |
| `ReadString$(stream)` | An Int length, then that many bytes | `stream.ReadString()` | compat (E13) | Bytes decoded as Windows-1252. "" if the length can't be read; a negative or huge length made Blitz allocate garbage or crash: compat gives "" and moves to the end. Short data gives what Blitz did, "" if the bytes can't all be read. |
| `ReadLine$(stream)` | Bytes up to a \n, without it; every \r anywhere in the line is dropped | `stream.ReadLine()` | compat (E13) | Not only a \r before the \n: all of them (`if( c!='\r' )`). Gives "" at the end, never Null. std's `BufferedReader.ReadLine` keeps a lone \r and gives Null. |
| `WriteByte stream,byte` | The low byte | `stream.WriteByte(n)` | compat (E13) | |
| `WriteShort stream,short` | The low 2 bytes | `stream.WriteShort(n)` | compat (E13) | |
| `WriteInt stream,int` | 4 bytes | `stream.WriteInt(n)` | compat (E13) | |
| `WriteFloat stream,float#` | 4 bytes of an IEEE single | `stream.WriteFloat(f)` | compat (E13), with `ToBits()` | |
| `WriteString stream,string$` | An Int length, then the bytes | `stream.WriteString(s)` | compat (E13) | Encoded as Windows-1252 (a character not in it as "?"), so the length is the character count and old save files read back. |
| `WriteLine stream,string$` | The bytes, then "\r\n" | `stream.WriteLine(s)` | compat (E13) | Always \r\n, on every OS. |
| `CopyStream src,dest[,buffer_size=16384]` | Copies in chunks while neither stream is at its end, stopping after a short read | `Blitz.CopyStream(from, to, size)` over std's `Copy(from, to)` | compat (E13), over std | Blitz checks `dest->eof()`, which is 1 for a WriteFile stream, so copying into a written file copied nothing; compat copies everything instead (design question 7). Debug builds rejected sizes outside 1 to 1048576. |

## Banks (bbbank.cpp)

A `BlitzBank` is a `Buffer<UInt8>` (std.ffi) that compat replaces on a Resize, so `Pointer()`
gives C its bytes, as a bank passed to a userlib's `*` parameter did.

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `CreateBank([size=0])` | size bytes, zeroed, capacity rounded up to 16 | `BlitzBank(size)` | compat (E13), over std's `Buffer<UInt8>` | |
| `FreeBank bank` | Frees it; an unknown or freed bank is ignored | `bank.Free()` | compat (E13) | Twice is fine, as in Blitz. Using it after is a panic ("bank does not exist", which Blitz's debug build raised). |
| `BankSize(bank)` | Its size | `bank.Size()` | compat (E13) | |
| `ResizeBank bank,size` | Keeps the bytes; new ones within the old capacity are zeroed | `bank.Resize(size)` | compat (E13) | Growing past the capacity (to max(1.5 times, size), rounded to 16) copies only the old bytes, and in release builds the new ones were whatever `new char[]` held. compat zeroes them, which is what a game could have seen at best. The data moves, so a Pointer from before is stale, as in Blitz. |
| `CopyBank src,src_offset,dest,dest_offset,count` | `memmove`, so overlapping ranges are fine | `BlitzBank.Copy(src, srcOffset, dest, destOffset, count)` | compat (E13) | |
| `PeekByte(bank,offset)` | A byte, 0 to 255 | `bank.PeekByte(offset)` | compat (E13) | |
| `PeekShort(bank,offset)` | 2 bytes, unsigned | `bank.PeekShort(offset)` | compat (E13) | Little-endian, any offset (unaligned is fine). |
| `PeekInt(bank,offset)` | 4 bytes, signed | `bank.PeekInt(offset)` | compat (E13) | |
| `PeekFloat#(bank,offset)` | 4 bytes as an IEEE single | `bank.PeekFloat(offset)` | compat (E13), with `Float32.FromBits` | |
| `PokeByte bank,offset,value` | The low byte | `bank.PokeByte(offset, value)` | compat (E13) | |
| `PokeShort bank,offset,value` | The low 2 bytes | `bank.PokeShort(offset, value)` | compat (E13) | Blitz's debug check tests only `offset`, not `offset+1`, and release checks nothing; compat checks the whole width (design question 6). |
| `PokeInt bank,offset,value` | 4 bytes | `bank.PokeInt(offset, value)` | compat (E13) | As PokeShort: up to 3 bytes past the end went into the 16-byte padding unnoticed. |
| `PokeFloat bank,offset,value#` | 4 bytes of an IEEE single | `bank.PokeFloat(offset, value)` | compat (E13), with `ToBits()` | As PokeInt. |
| `ReadBytes(bank,file,offset,count)` | Reads up to count bytes from a stream into the bank, giving how many | `stream.ReadBytes(bank, offset, count)` | compat (E13), over std's `Reader.Read` | Short at the end; the count says how many. |
| `WriteBytes(bank,file,offset,count)` | Writes count bytes of the bank to a stream, giving how many | `stream.WriteBytes(bank, offset, count)` | compat (E13), over std's `Writer.Write` | |
| `CallDLL(dll_name$,func_name$[,in_bank][,out_bank])` | Loads a DLL by name (cached), calls `int func(const void* in, int in_size, void* out, int out_size)`, gives its Int; 0 if either isn't found | An `Extern "C" Lib "name"` Function of that signature, called with `bank.Pointer()` and `bank.Size()` | language when both names are literals; dropped otherwise | FutureBlitz is 64-bit only, so the game's 32-bit DLLs never load: each needs a 64-bit build or a replacement (most were what fbengine now does). A name built at run time needs `DynamicLibrary` (Deferred work in milestones.md): dropped until then, with a TODO comment. Blitz restored esp after the call, so cdecl and stdcall both worked; 64-bit has one convention. |

## User libraries (userlibs.cpp)

Every symbol here is internal. The user-visible feature is .decls files.

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| a userlib's .decls (`.lib "x.dll"`, `Func%(a%, b#, s$, p*):"Symbol"`) | Declares a DLL's functions, loaded at start (`_bbLoadLibs`); a missing DLL or symbol is an error only when called | An `Extern "C" Lib "x"` block: `%` Int32, `#` Float32, `$` CString, `*` `Ptr<Void>` (a bank passed as `bank.Pointer()`, a Type as a `@CLayout` Struct) | language | As CallDLL, the DLL needs a 64-bit build, and FutureBlitz links it at build time, so a missing symbol is a build error, not a call-time one. Blitz's CStrings lived in a ring of 256 buffers (`_bbStrToCStr`), so a DLL could keep one for a while; FutureBlitz's last only for the call. |

---

## Language constructs behind basic.txt's helpers

basic.txt's `_bb` names are the compiled code's runtime: strings, arrays, Type objects, Data, and
the operators. None is called by name, but each is a construct the migrator must translate, so
they're mapped here by construct. Goto and Gosub have no helper here; the migrator's structuring
and the AI pass handle them.

| Blitz3D | What it does | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `Int(x#)`, and every Float to Int conversion: assignment to an Int, an Int parameter (`_bbStrToInt`, `IR_CAST`) | `fistp` under round-to-nearest: halves go to even | `Blitz.Int(f)` | compat (E13) | `Int(2.5)` is 2, `Int(3.5)` is 4, `Int(-2.5)` is -2. FutureBlitz's `As Int` truncates and panics out of range; Blitz gives -2147483648 for NaN, Inf, and out of range (the FPU's "integer indefinite"). Constant folding does the same (`FloatConstNode::intValue`). Design question 8 (a std `RoundEven`). |
| `Int(s$)`, string to Int conversions | C's `atoi`: leading whitespace, a sign, digits, the rest ignored; 0 if none | `Blitz.Int(s)` | compat (E13) | `"12abc"` is 12, `" 7"` is 7, `"abc"` 0, `"0x10"` 0. Overflow is undefined in atoi (MSVC's gave the wrapped value); `Int.Parse` gives Null for all but the plain cases. |
| `Float(s$)`, string to Float | C's `atof`, the same leniency, exponents allowed | `Blitz.Float(s)` | compat (E13) | `Float.Parse` gives Null for trailing junk. |
| `Float(i)` | Int to Float | `i As Float` | language | Rounds past 2^24 in both. |
| `Str(i)`, Int to String (concatenation, Print) | `itoa` | `"{i}"` | language | Same digits. |
| `Str(f#)`, and every Float to String: `"x=" + f#`, Print, Read into a String (`ftoa`) | 6 significant digits, `_ecvt`; between 1e-4 and 1e8 written without an exponent and with at least one digit after the point; trailing zeros trimmed | `Blitz.Str(f)` | compat (E13) | 1.0 is "1.0", 0.1 "0.1", 1.0/3 "0.333333", 0 "0.0", 123456.0 "123456.0", 1234567.0 "1234570.0" (rounded to 6 digits, padded with 0), 0.001 "0.001". Outside the range `_gcvt(n, 6)`, MSVC's form: 123456789.0 is "1.23457e+008", 0.00001 "1.e-005" (exponent of 3 digits; check these against a real Blitz3D build in compat's tests). NaN "NaN", Inf "Infinity" and "-Infinity". FutureBlitz's interpolation writes the shortest round-trip form ("1", "0.33333334"), so every float that reaches a string goes through Blitz.Str. |
| `Str(obj)`, a Type object to String (`_bbObjToStr`) | `[1,2.5,"text",[...]]`: each field in order, nested objects to 8 levels, `[NULL]` for Null or deleted, `[ROOT]` for a cycle back to the first, `....` past 8 | `Blitz.Str(o)` | compat (E13), over the language's reflection | Floats as Blitz.Str. |
| `Abs(n)` (`_bbAbs`, `_bbFAbs`) | Absolute value, Int or Float | `Abs(n)` | std | `Abs(-2147483648)` wraps to itself in Blitz; std's overflows, which traps in debug builds. |
| `Sgn(n)` (`_bbSgn`, `_bbFSgn`) | -1, 0, or 1, as the argument's type | `Sign(n)` | std | NaN gives NaN in std; Blitz's `_bbFSgn` gave 0. |
| `a Mod b`, Int (`_bbMod`) | C's `%`: the remainder has a's sign | `a % b` | language | `-7 Mod 3` is -1. Not std.math's `Mod`, which takes b's sign. By 0: "Integer divide by zero" in Blitz, a panic in FutureBlitz. |
| `a Mod b`, Float (`_bbFMod`) | `fmod`, a's sign | `a % b` (or `Rem(a, b)`) | language | |
| `a ^ b` (`_bbFPow`) | Always Float `pow`, whatever the operands | `(a As Float) ** (b As Float)` | language | `2 ^ 3` is the Float 8.0 in Blitz; assigned to an Int it goes through Blitz.Int. |
| `a / b`, two Ints | `idiv`, toward zero | `a / b` | language | Same. Int and Float mixed is Float in Blitz; FutureBlitz needs the cast (`Int + Float` is an error). |
| `Shl`, `Shr`, `Sar` | Shift left, logical right, arithmetic right; counts taken mod 32 (x86) | `<<`, `((x As UInt32) >> n) As Int`, `>>` | language | FutureBlitz's `>>` is arithmetic on Int, so Shr needs the UInt32. A count outside 0 to 31 panics in debug builds; release masks it as x86 did. |
| Int overflow anywhere | Wraps | the same operators | language | FutureBlitz traps overflow in debug builds; seed and hash code (`h = h * 31 + c`) that relies on wrapping needs `WrappingMultiply`/`WrappingAdd` (design question 9). |
| `And`, `Or`, `Xor`, `Not`, `True`, `False`, comparisons | And/Or/Xor bitwise on Ints, not short-circuit; Not logical (`Not 5` is 0); comparisons give 1 or 0; True is 1 | `&&`/`\|\|`/`!` where both sides are truthy tests, `&`/`\|`/`^` otherwise, `(cond) As Int` where a number is wanted | language | Blitz always evaluated both sides of And/Or, so a translation to `&&` changes behavior only if the right side has side effects; the migrator keeps `&`-style when it does. |
| `Dim a(n[,m...])`, `a(i)` (`_bbDimArray`, `_bbUndimArray`, `_bbArrayBoundsEx`) | Global arrays of n+1 by m+1 elements, zeroed; Dim again throws the old contents away | `Global a: Int[]` made with n+1 zeros, or a `Grid<T>` (std.collections) for two dimensions | language / std | Indices 0 to n inclusive. Debug builds checked only the flattened index (`ArrayVarNode::translate`), so `a(x, y)` with y past m read the next row unnoticed, and release checked nothing; FutureBlitz panics on every out-of-range index. The migrator can't fix such latent bugs, only report them when they panic. |
| `Local a[n]`, `Field a[n]` (Blitz arrays, `_bbVecAlloc`) | Fixed arrays of n+1 elements, sizes constant | `Local a: Int[n + 1]` | language | Again n+1. Debug-only bounds checks in Blitz. |
| `Type T ... Field ... End Type` | A record type whose objects are all in one list, in creation order | `Class T Extends BlitzObject`, with `Static Field List = BlitzList<T>()` | compat (E13) | BlitzObject holds the deleted flag and the handle. |
| `New T` (`_bbObjNew`) | A zeroed object added to the end of T's list | `T.List.New()` (makes `T()` and adds it) | compat (E13) | Blitz arrays in fields are allocated zeroed; Strings start "". |
| `Delete o` (`_bbObjDelete`) | Releases its fields and handle, marks it deleted; every other reference to it now compares equal to Null; it leaves the list once nothing refers to it | `o.Delete()` | compat (E13) | The deleted object stays in memory while referenced, as in Blitz. Reading a field of a deleted object was "Object does not exist"; compat panics the same way only where it checks (design question 10). |
| `Delete Each T` | Deletes every object of T | `T.List.DeleteEach()` | compat (E13) | |
| `For o = Each T ... Next` (`_bbObjEachFirst`, `_bbObjEachNext`) | Walks T's list in order, skipping deleted objects; deleting any object, the current one included, during the walk is fine | `For o In T.List ... End For` | compat (E13) | FutureBlitz's collections panic when changed during a `For ... In` (only Pool doesn't, and it doesn't keep Blitz's order), and LinkedList's node walk breaks when the next node is removed. So BlitzList is a linked list of its own, unlinking lazily as Blitz did. Objects added during the walk are visited (they go at the end). |
| `First T`, `Last T` (`_bbObjFirst`, `_bbObjLast`) | First or last live object, or Null | `T.List.First()`, `T.List.Last()` | compat (E13) | A `T?`. |
| `After o`, `Before o` (`_bbObjNext`, `_bbObjPrev`) | The next or previous live object, or Null | `o.After()`, `o.Before()` (Methods BlitzObject gives each T, typed) | compat (E13) | Skips deleted objects. |
| `Insert a Before b`, `Insert a After b` (`_bbObjInsBefore`, `_bbObjInsAfter`) | Moves a next to b in the list | `T.List.InsertBefore(a, b)`, `T.List.InsertAfter(a, b)` | compat (E13) | a == b does nothing. |
| `o = Null`, `a = b` on objects (`_bbObjCompare`) | Compares the objects' field blocks, so any deleted object equals Null and every other deleted object | `Blitz.IsNull(o)`, `Blitz.Same(a, b)` | compat (E13) | Classic Blitz code relies on `If target <> Null` turning False once the target is deleted elsewhere; with plain FutureBlitz references it stays True and the game misbehaves silently. |
| `Handle(o)` (`_bbObjToHandle`) | A unique Int for the object, from 1 up, the same each time, never reused; 0 for Null or deleted | `Blitz.Handle(o)` | compat (E13) | Games keep handles in Ints, banks, and entity names, so they stay Ints (a `Map<Int, BlitzObject>` in compat), not std's `Handle<T>`. |
| `Object.T(h)` (`_bbObjFromHandle`) | The object for a handle, or Null if it's deleted, unknown, or of another Type | `Blitz.Object<T>(h)` | compat (E13) | |
| `Data`, `Read v`, `Restore [label]` (`_bbRestore`, `_bbReadInt`, `_bbReadFloat`, `_bbReadStr`) | A program-wide list of constants, read in order into variables, converted to the variable's type; Restore moves to a label's Data | A `Global` `BlitzData` holding every Data value in source order with each label's index; `Read x` becomes `x = data.ReadInt()` (or ReadFloat, ReadString), `Restore lbl` `data.Restore(LBL)` | compat (E13) | Conversions: a Float read into an Int truncates (C's conversion in `_bbReadInt`), unlike assignment, which rounds; a String into an Int is atoi; an Int or Float into a String is itoa or Blitz.Str. Past the end: "Out of data". Without a Restore, Read starts at the program's first Data. The AI pass can turn tables of Data into array literals. |
| `RuntimeStats` | Writes the live string, object, and unreleased-object counts to the debug log | nothing | dropped | FutureBlitz's debug builds report leaks at the end on their own (spec 08); the migrator emits a comment. |
| String concatenation and comparison with numbers (`_bbStrConcat`, `_bbStrCompare`) | `"a" + 5` is "a5"; a String compared with a number compares as Strings, bytewise | interpolation (`"a{n}"`, floats as `"{Blitz.Str(f)}"`), String `<` | language | `"10" < "9"` is True in both. Byte order equals code-point order for UTF-8. |

---

## Behaviors to match exactly

1. **Float to Int rounds half to even, saturating to -2147483648.** `fistp` under
   `_RC_NEAR` (bbruntime_dll.cpp `Runtime::execute` sets `_control87(_RC_NEAR|_PC_24|...)`;
   compiler/codegen_x86/codegen_x86.cpp `IR_CAST`; compiler/exprnode.cpp
   `FloatConstNode::intValue` for constants). Every implicit conversion, not only `Int()`.
2. **But Data's Float-to-Int truncates.** bbruntime/basic.cpp `_bbReadInt` returns a float
   from an int function, which C converts toward zero. So do `Rand` (bbmath.cpp `bbRand`,
   `int(rnd()*...)`).
3. **Float math is single precision throughout** (`_PC_24`), every FPU exception masked:
   division by zero gives Inf, not an error. Integer division by zero is "Integer divide by
   zero" (bbruntime_dll.cpp `seTranslator`).
4. **Str of a Float is 6 significant digits** with Blitz's layout (stdutil/stdutil.cpp `ftoa`):
   fixed between 1e-4 and 1e8 with a ".0" minimum, else MSVC's `_gcvt` with a 3-digit exponent.
5. **The random generator**: Park-Miller with A=48271, Schrage's method, seeded 0x1234 at
   start, `(state & 65535)/65536 + 0.5/65536`, so 65536 values that never reach 0 or 1;
   `Rnd(x)` is `Rnd(x, 0)`, `Rand(x)` is `Rand(1, x)` with the ends swapped when needed;
   `SeedRnd` masks to 31 bits and turns 0 into 1; `RndSeed` gives the state (bbmath.cpp `rnd`,
   `bbRnd`, `bbRand`, `bbSeedRnd`, `bbRndSeed`, `math_create`).
6. **Trig in degrees**, inverse trig giving degrees, `ATan2(y, x)` (bbmath.cpp).
7. **Mod has the dividend's sign**, Int and Float (basic.cpp `_bbMod`, `_bbFMod`); `^` is always
   a Float `pow` (compiler/exprnode.cpp `ArithExprNode::semant`).
8. **Strings are 1-based** with clamping and always-on errors: `Mid` and `Instr` reject a start
   below 1, `Left`, `Right`, `LSet`, `RSet` a negative count; past-the-end starts give "" or 0
   (bbstring.cpp `CHKPOS`, `CHKOFF`, `bbMid`, `bbInstr`, `bbLeft`, `bbRight`, `bbLSet`,
   `bbRSet`). `Instr(s, "")` gives `from`. `Asc("")` is -1. `Hex` and `Bin` are always 8 and 32
   digits.
9. **Upper, Lower, and Trim are ASCII and C-locale** (bbstring.cpp `bbUpper`, `bbLower`,
   `bbTrim` with `isgraph`).
10. **atoi and atof leniency** for every String to number conversion (basic.cpp
    `_bbStrToInt`, `_bbStrToFloat`).
11. **Type objects**: creation order, Insert moves, deleted objects compare equal to Null and
    are skipped by First, Last, After, Before, and Each, deletion during Each is safe, handles
    count up from 1 and are never reused, `Object.T` checks the type (basic.cpp `_bbObjNew`,
    `_bbObjDelete`, `_bbObjCompare`, `_bbObjNext`, `_bbObjPrev`, `_bbObjInsBefore`,
    `_bbObjInsAfter`, `_bbObjEachFirst`, `_bbObjToHandle`, `_bbObjFromHandle`).
12. **Arrays have n+1 elements**, zeroed; Dim again clears (basic.cpp `_bbDimArray`,
    compiler/declnode.cpp `sizes.push_back( n+1 )`).
13. **Streams**: little-endian; ReadByte and ReadShort unsigned; reads at the end give 0 or
    partial values, no error; ReadLine drops every \r and ends at \n; WriteLine writes \r\n;
    ReadString and WriteString are an Int length then bytes (bbstream.cpp).
14. **Files**: OpenFile opens existing files for read and write without truncating; every
    failure gives 0; FileSize gives 0 for folders and missing files; FileType 0, 1, 2; CurrentDir
    and SystemProperty folders end with a separator; NextFile gives "." and ".." and "" at the
    end (bbfilesystem.cpp, gxruntime/gxfilesystem.cpp `getCurrentDir`, `getFileSize`,
    `getFileType`, gxdir.cpp `getNextFile`, gxruntime.cpp `systemProperty`).
15. **Banks**: zeroed on creation and on growth within capacity; PeekByte and PeekShort
    unsigned; unaligned little-endian access at any offset; CopyBank overlap-safe; FreeBank
    idempotent (bbbank.cpp).
16. **Timers tick every `1000/hertz` whole milliseconds** and WaitTimer gives the ticks since the
    last wait (gxruntime/gxtimer.cpp `gxTimer::gxTimer`, `gxTimer::wait`).
17. **MilliSecs wraps as a 32-bit Int** (gxruntime.cpp `getMilliSecs`, `timeGetTime`).
18. **The window's close button ends the program** at the next Delay, WaitTimer, Flip, or other
    `idle()` call, unless AppTitle's prompt is cancelled (gxruntime.cpp `WM_CLOSE`,
    `gxRuntime::idle`; bbruntime.cpp `bbDelay`, `bbWaitTimer`).
19. **RuntimeError and every runtime error show a message box** titled "Error!" in a release
    build, then end the program (bbruntime_dll.cpp `DummyDebugger::debugMsg`); DebugLog and Stop
    do nothing there.

## Design questions

1. **Where does the compat module live?** It's needed at run time by every migrated game, so not
   in `fbengine_tools` (build-time), and it isn't fbengine's (strings, Data, Types aren't the
   engine's). Recommend a sixth package, `fbengine_blitz`, pure FutureBlitz with no native code,
   released with the others and built in E13, depending only on std (and on fbengine for
   RuntimeError's message box, or not at all if that box goes through a callback fbengine sets).
2. **Strings: bytes or characters?** Blitz strings were Windows-1252 bytes. Recommend: the
   migrator converts .bb source to UTF-8; compat's string functions count code points, so `Len`,
   `Mid`, `Instr`, `Chr`, and `Asc` mean what they meant for every Windows-1252 character; file
   text (ReadLine, ReadString, WriteLine, WriteString) is decoded and encoded with std.text's
   `Decode`/`Encode` and `TextEncoding.WINDOWS_1252`, so old data files and saves read the same.
   compat keeps an ASCII fast path (Length == CharCount) for speed. The other choice, byte
   positions over UTF-8, breaks on the first "ü" in a German game's text.
3. **Paths.** Blitz games use `"GFX\map\Room1.b3d"`: backslashes and Windows' case-insensitivity.
   Recommend compat's `Blitz.Path(p)` turns backslashes into slashes and, on Linux and macOS,
   falls back to a case-insensitive match when the exact path isn't there (cached per folder), and
   that the migrator routes every path argument through it; assets loaded through fbengine get the
   same rule in E8's `res://` resolution. A one-time rename tool in fbengine_tools could fix a
   game's files and source together for those who want it clean.
4. **Closing the window, and Delay.** Blitz ended the program from inside any `idle()` call;
   fbengine's `Window.Closed()` has to be polled. Recommend fbengine E1 add
   `Window.SetEndOnClose(prompt)` (off by default; the migrator turns it on with AppTitle's
   prompt or ""), which Flip honors by ending the program, and that compat's `Blitz.Delay` and
   `BlitzTimer.Wait` sleep in slices of 100 ms at most, letting fbengine read the OS's events
   between them (a small fbengine E1 `PumpEvents()` that reads events without ending the frame),
   so long Delays don't make the window "not responding".
5. **RuntimeError's message box.** Recommend fbengine E1 `ShowMessage(title, text, kind)` over
   `SDL_ShowSimpleMessageBox` (useful beyond migration), and compat's `Blitz.RuntimeError` shows
   "Error!" with the message and exits with 1. A `Panic` would lose the message for players.
6. **Faithful to Blitz's missing checks?** Blitz's release builds checked no bank offsets, array
   indices, or handles, so shipped games may read out of range unnoticed. Recommend compat and
   the language panic on every one (with the Blitz-style message), and the migrator's docs say so;
   reproducing reads of garbage is neither possible nor wanted. Pokes up to 3 bytes past a bank's
   end landed in its 16-byte padding: recommend compat also gives every bank Blitz's padding
   (capacity rounded to 16) and lets Peek and Poke reach it, since that one is harmless and real
   games hit it.
7. **Blitz's bugs: keep or fix?** CopyStream into a WriteFile stream copies nothing; `SeedRnd(-1)`
   makes Rnd constant; Replace with an empty `from` hangs. Recommend fixing what can only have been
   a bug that a game would have noticed (CopyStream copies; Replace with "" returns s), and
   keeping what changes a game's output silently (the generator, exactly).
8. **A std `RoundEven`.** Every Float to Int conversion needs round-half-to-even; LLVM has
   `llvm.roundeven`, one instruction. Recommend adding `RoundEven` to std/math/scalar.fb beside
   Round (it's general, C's `nearbyint`), so `Blitz.Int(f)` is `RoundEven(f)` plus the range
   check; without it compat writes it by hand.
9. **Wrapping arithmetic.** Blitz wrapped every Int overflow; FutureBlitz traps in debug builds.
   Recommend the migrator emit plain operators and its tests run migrated games in debug builds,
   with the AI pass turning each trap it finds into `WrappingAdd`/`WrappingMultiply`; the
   alternative, wrapping every operation, makes the translated code unreadable.
10. **Deleted objects' fields.** Blitz raised "Object does not exist" reading a deleted object's
    field. Checking on every field access costs a branch each; recommend compat checks only in
    debug builds (a `DebugAssert` in generated accessors is too heavy, so: `Blitz.IsNull` at each
    `= Null` the migrator sees, and a debug-build check in `After`, `Before`, `Insert`, and
    `Handle`), and fields of a deleted object read as their zeroes, as `_bbObjDelete` released
    them.
11. **Random: exact or std?** Recommend compat's exact generator by default, since seeded level
    generation must match, with a migrator option to emit std's `RandInt`/`RandFloat`/
    `SeedRandom` (PCG32) for games that don't care.

## Counts

Commands in the eight lists that aren't internal: 99 (basic 1, bbbank 16, bbfilesystem 17,
bbmath 17, bbruntime 15, bbstream 15, bbstring 18, userlibs 0).

| Where | Count | Commands |
|---|---|---|
| std | 34 | End, Stop, Delay, GetEnv, SetEnv, DebugLog; Sin, Cos, Tan, ASin, ACos, ATan, ATan2, Sqr, Floor, Ceil, Exp, Log, Log10; String, Replace, Upper, Lower, Trim, Len, CurrentDate, CurrentTime; ChangeDir, CreateDir, DeleteDir, FileSize, FileType, CopyFile, DeleteFile |
| language | 2 | Hex, Bin |
| fbengine E1 | 1 | AppTitle (SetTitle exists; the close prompt is new) |
| compat (E13) | 60 | RuntimeError, ExecFile, MilliSecs, CommandLine, SystemProperty, CreateTimer, WaitTimer, FreeTimer; Rnd, Rand, SeedRnd, RndSeed; Left, Right, Instr, Mid, LSet, RSet, Chr, Asc; OpenFile, ReadFile, WriteFile, CloseFile, FilePos, SeekFile, ReadDir, CloseDir, NextFile, CurrentDir; all 15 of bbstream; CreateBank to WriteBytes, 15 of bbbank |
| language, or dropped | 1 | CallDLL (an Extern Function when its names are literals, dropped otherwise) |
| dropped | 1 | RuntimeStats |

By file: bbruntime std 6, compat 8, fbengine E1 1; bbmath std 13, compat 4; bbstring std 8,
compat 8, language 2; bbfilesystem std 7, compat 10; bbstream compat 15; bbbank compat 15, CallDLL
1; basic dropped 1. Total 99.

The basic.txt section's constructs, besides RuntimeStats: 31 rows, compat 17, language 11, std 2
(Abs, Sgn), and Dim (language arrays or std's Grid) 1.

### Which need compat and which are plain std

- **Plain std or language, one exact call:** End, Stop, Delay, GetEnv, SetEnv, DebugLog, every
  trig and math function but the random ones, String, Replace (with a non-empty literal), Upper,
  Lower, Trim, Len, Hex, Bin, CurrentDate, CurrentTime, ChangeDir, CreateDir, DeleteDir,
  FileSize, FileType, CopyFile, DeleteFile, Abs, Sgn, Mod, `^`, `/`, the shifts, Float(i), Str(i),
  Dim and Blitz arrays.
- **Compat, because std's shape or edge cases differ:** Int(f) and every Float-to-Int,
  Int(s), Float(s), Str(f) and every Float-to-String, Str(obj); Rnd, Rand, SeedRnd, RndSeed;
  Left, Right, Mid, Instr, LSet, RSet, Chr, Asc (and Replace with a non-literal `from`);
  RuntimeError, ExecFile, MilliSecs, CommandLine, SystemProperty, CurrentDir; the timers; every
  stream read and write, the file opens and their handles, ReadDir and NextFile; every bank
  command; Type lists (New, Delete, Delete Each, Each, First, Last, After, Before, Insert, Null
  comparisons), Handle and Object; Data, Read, Restore.

## Skipped (internal)

- basic.txt (51): `_bbIntType`, `_bbFltType`, `_bbStrType`, `_bbCStrType`, `_bbStrLoad`,
  `_bbStrRelease`, `_bbStrStore`, `_bbStrCompare`, `_bbStrConcat`, `_bbStrToInt`,
  `_bbStrFromInt`, `_bbStrToFloat`, `_bbStrFromFloat`, `_bbStrConst`, `_bbDimArray`,
  `_bbUndimArray`, `_bbArrayBoundsEx`, `_bbVecAlloc`, `_bbVecFree`, `_bbVecBoundsEx`, `_bbObjNew`,
  `_bbObjDelete`, `_bbObjDeleteEach`, `_bbObjRelease`, `_bbObjStore`, `_bbObjCompare`,
  `_bbObjNext`, `_bbObjPrev`, `_bbObjFirst`, `_bbObjLast`, `_bbObjInsBefore`, `_bbObjInsAfter`,
  `_bbObjEachFirst`, `_bbObjEachNext`, `_bbObjEachFirst2`, `_bbObjEachNext2`, `_bbObjToStr`,
  `_bbObjToHandle`, `_bbObjFromHandle`, `_bbNullObjEx`, `_bbRestore`, `_bbReadInt`,
  `_bbReadFloat`, `_bbReadStr`, `_bbAbs`, `_bbSgn`, `_bbMod`, `_bbFAbs`, `_bbFSgn`, `_bbFMod`,
  `_bbFPow`. Their constructs are mapped in the basic.txt section above.
- bbruntime.txt (3): `_bbDebugStmt`, `_bbDebugEnter`, `_bbDebugLeave` (the IDE debugger's hooks;
  FutureBlitz's debugger works on DWARF and CodeView).
- userlibs.txt (3): `_bbLoadLibs`, `_bbStrToCStr`, `_bbCStrToStr` (the .decls loader and its
  string conversions; mapped above as Extern blocks).
