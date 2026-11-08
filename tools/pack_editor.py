"""Builds fbengine_editor for a target, as a release's programs are, and packs it for the tag's GitHub
release: the program by its OS's name, the DLLs fb build copies beside it, the README and LICENSE,
and notices/, which has each library fb build says it bundled (its "Bundled <library> ...: <path>"
lines) and each copied DLL's package's notices, in notices/<package>/; not the program's own
debug symbols. It refuses an archive with a
program that isn't its OS's, a DLL no package has, or a bundled library without its notice, and
prints the archive's path last.

    python tools/pack_editor.py <fb> <version> <target> <folder to pack in> [the repository's folder]
"""

import pathlib
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile

TARGETS = ("windows-x64", "linux-x64", "linux-arm64", "macos-arm64", "macos-x64")
# What each OS's programs start with: PE's MZ, ELF's, and 64-bit Mach-O's, either way round.
MAGIC = {"windows": (b"MZ",), "linux": (b"\x7fELF",), "macos": (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf")}
BUNDLED = re.compile(r"^\s*Bundled (.+?), under .*?: (.+?)\s*$", re.MULTILINE)


def program_name(target):
    return "fbengine_editor.exe" if target.startswith("windows") else "fbengine_editor"


def build(fb, root, program):
    """fb build's release of the editor, at `program`: its exit code and what it said."""
    done = subprocess.run([fb, "--color", "never", "build", str(root / "packages" / "fbengine_editor"), "--release",
                           "-o", str(program)], capture_output=True, text=True, encoding="utf-8", errors="replace")
    return done.returncode, done.stdout + done.stderr


def pack(root, version, target, out, built):
    """The archive's path, or what's wrong with it. `built(program)` builds the program there and
    gives its exit code and what it said."""
    if target not in TARGETS:
        return None, f"there's no target {target}"
    name = f"fbengine_editor-{version}-{target}"
    folder = out / name
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    program = folder / program_name(target)
    code, said = built(program)
    if code != 0:
        return None, f"fb build failed:\n{said[-3000:]}"
    os = target.split("-")[0]
    if not program.is_file():
        found = ", ".join(sorted(path.name for path in folder.iterdir())) or "nothing"
        return None, f"there's no {program.name}, {os}'s name for the program, in what fb build made: {found}"
    with program.open("rb") as file:
        start = file.read(4)
    if not start.startswith(MAGIC[os]):
        return None, f"{program.name} isn't a {os} program: it starts {start!r}"
    notices = folder / "notices"
    notices.mkdir()
    bundled = BUNDLED.findall(said)
    if not bundled:
        return None, "fb build named no library it bundled, as its runtime's are:\n" + said[-3000:]
    for library, path in bundled:
        source = pathlib.Path(path)
        if not source.is_file():
            return None, f"fb build bundled {library}, whose notice {path} isn't there"
        if (notices / source.name).exists():
            return None, f"two bundled libraries' notices are both {source.name}"
        shutil.copyfile(source, notices / source.name)
    packages = sorted(path for path in (root / "packages").iterdir() if (path / "native" / target).is_dir())
    # The program's own debug symbols, macOS's .dSYM folder and Windows' .pdb, stay out of the
    # release's archive.
    symbols = {f"{program.name}.dSYM", f"{program.stem}.pdb"}
    for path in sorted(folder.iterdir()):
        if path in (program, notices):
            continue
        if path.name in symbols:
            if path.is_dir():
                shutil.rmtree(path)
            else:
                path.unlink()
            continue
        owners = [package for package in packages if (package / "native" / target / path.name).is_file()]
        if not owners:
            return None, f"fb build put {path.name} beside the program, which no package's native/{target} has"
        for package in owners:
            if not (package / "notices").is_dir():
                return None, f"{package.name}, whose {path.name} the program needs, has no notices"
            shutil.copytree(package / "notices", notices / package.name, dirs_exist_ok=True)
    for library, path in bundled:
        if not (notices / pathlib.Path(path).name).is_file():
            return None, f"notices/ has nothing of {library}"
    for each in ("README.md", "LICENSE"):
        shutil.copyfile(root / "packages" / "fbengine_editor" / each, folder / each)
    if os == "windows":
        archive = out / f"{name}.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zipped:
            for path in sorted(folder.rglob("*")):
                zipped.write(path, path.relative_to(out).as_posix())
    else:
        archive = out / f"{name}.tar.gz"
        program.chmod(0o755)
        with tarfile.open(archive, "w:gz") as tarred:
            tarred.add(folder, name)
    return archive, None


def main():
    if len(sys.argv) not in (5, 6):
        print(__doc__)
        return 2
    fb, version, target, out = sys.argv[1], sys.argv[2], sys.argv[3], pathlib.Path(sys.argv[4]).resolve()
    here = pathlib.Path(__file__).resolve().parent.parent
    root = pathlib.Path(sys.argv[5]).resolve() if len(sys.argv) == 6 else here
    archive, wrong = pack(root, version, target, out, lambda program: build(fb, root, program))
    if wrong:
        print(wrong)
        return 1
    print(f"packed the editor, its DLLs, and the notices of what they bundle: {archive}")
    print(archive)
    return 0


if __name__ == "__main__":
    sys.exit(main())
