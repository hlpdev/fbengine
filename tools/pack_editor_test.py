"""Tests pack_editor.py with a build of its own, in a temporary folder: one that makes what fb build
makes, packed whole for each OS, then each way it can fall short.

    python tools/pack_editor_test.py
"""

import pathlib
import sys
import tarfile
import tempfile
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import pack_editor  # noqa: E402

PROGRAMS = {"windows": b"MZ\x90\x00", "linux": b"\x7fELF", "macos": b"\xcf\xfa\xed\xfe"}
LIBRARIES = {"windows": "fbengine_native.dll", "linux": "libfbengine_native.so", "macos": "libfbengine_native.dylib"}


def made(root, target):
    """A repository whose fbengine has a DLL for `target` and notices, and an fb whose notices are
    there."""
    os = target.split("-")[0]
    package = root / "packages" / "fbengine"
    (package / "native" / target).mkdir(parents=True)
    (package / "native" / target / LIBRARIES[os]).write_bytes(b"its code")
    (package / "notices").mkdir()
    (package / "notices" / "sdl.txt").write_text("sdl's license\n", encoding="utf-8")
    editor = root / "packages" / "fbengine_editor"
    editor.mkdir(parents=True)
    (editor / "README.md").write_text("the editor\n", encoding="utf-8")
    (editor / "LICENSE").write_text("Apache-2.0\n", encoding="utf-8")
    (root / "fb" / "notices").mkdir(parents=True)
    (root / "fb" / "notices" / "libuv.txt").write_text("libuv's license\n", encoding="utf-8")


def builder(root, target, program_as=None, start=None, beside=None, bundled=None, code=0, symbols=False):
    """What fb build does, made as told otherwise."""
    os = target.split("-")[0]

    def built(program):
        named = program.with_name(program_as) if program_as else program
        named.write_bytes(start if start is not None else PROGRAMS[os] + b"the rest")
        for name in beside if beside is not None else [LIBRARIES[os]]:
            (program.parent / name).write_bytes(b"its code")
        if symbols:
            # A program's debug symbols, as macOS's fb build makes them: a folder beside it.
            dsym = program.parent / f"{program.name}.dSYM" / "Contents" / "Resources" / "DWARF"
            dsym.mkdir(parents=True)
            (dsym / program.name).write_bytes(b"its symbols")
            (program.parent / f"{program.stem}.pdb").write_bytes(b"its symbols")
        lines = bundled if bundled is not None else [
            f"     Bundled libuv 1.52.1, under the MIT license; ship its notices with the program: "
            f"{(root / 'fb' / 'notices' / 'libuv.txt').as_posix()}"]
        return code, "    Finished release build in 1s\n" + "\n".join(lines) + "\n"

    return built


def names(archive):
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as zipped:
            return set(zipped.namelist())
    with tarfile.open(archive) as tarred:
        return set(tarred.getnames())


def check(name, target, expected, absent=(), **how):
    with tempfile.TemporaryDirectory() as folder:
        root = pathlib.Path(folder) / "repository"
        made(root, target)
        archive, wrong = pack_editor.pack(root, "1.0.0", target, pathlib.Path(folder) / "out",
                                          builder(root, target, **how))
        held = names(archive) if archive else set()
    top = f"fbengine_editor-1.0.0-{target}/"
    program = top + pack_editor.program_name(target)
    if expected:
        ok = archive is None and expected in (wrong or "")
    else:
        ok = wrong is None and {program, top + "notices/libuv.txt", top + "notices/fbengine/sdl.txt",
                                top + "README.md", top + "LICENSE"} <= held
        ok = ok and not any(each.startswith(top + gone) for each in held for gone in absent)
    print(f"{name}: {'ok' if ok else 'WRONG'}{'' if ok else ': ' + repr(wrong) + ' ' + repr(sorted(held))}")
    return ok


def main():
    results = [check(f"whole on {target}", target, None) for target in pack_editor.TARGETS]
    results += [
        check("windows' program without its .exe", "windows-x64", "there's no fbengine_editor.exe",
              program_as="fbengine_editor"),
        check("a linux program that's windows'", "linux-x64", "isn't a linux program", start=b"MZ\x90\x00"),
        check("a macos program that's linux'", "macos-arm64", "isn't a macos program", start=b"\x7fELF"),
        check("its debug symbols left out", "macos-arm64", None, symbols=True,
              absent=("fbengine_editor.dSYM", "fbengine_editor.pdb")),
        check("its debug symbols left out on windows", "windows-x64", None, symbols=True,
              absent=("fbengine_editor.exe.dSYM", "fbengine_editor.pdb")),
        check("its debug symbols and a file unknown", "macos-arm64", "fb build put notes.txt beside the program",
              symbols=True, beside=["libfbengine_native.dylib", "notes.txt"]),
        check("a DLL beside it no package has", "windows-x64", "which no package's native/windows-x64 has",
              beside=["fbengine_native.dll", "SDL3.dll"]),
        check("no bundled library named", "linux-x64", "named no library it bundled", bundled=[]),
        check("a bundled library whose notice isn't there", "linux-x64", "whose notice /nowhere/mbedtls.txt",
              bundled=["Bundled mbedTLS 4.1.1, under the Apache-2.0 license; ship its notices with the program: "
                       "/nowhere/mbedtls.txt"]),
        check("fb build failing", "windows-x64", "fb build failed", code=1),
    ]
    if not all(results):
        return 1
    print(f"all {len(results)} cases hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
