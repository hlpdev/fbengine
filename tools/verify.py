"""Checks fbengine before a step lands: every lane's check, and every landing's.

    python tools/verify.py [--build <folder>] [--base <ref>] [--no-build] [--no-tidy] [--only <regex>]

It builds (configuring the folder first if it's new), runs ctest (the C++ tests, the packages'
`fb test`, and the examples, every test offscreen), checks each package's DLL exports only its C
API, checks the bindings are what tools/bindgen.sh makes, runs clang-format on every C and C++
file, clang-tidy on the C++ files that changed since --base (all of an area's when its header
changed), and `fb fmt --check` on the packages and examples. It ends with "everything passed", or
how many checks failed; a landing's push waits on that line.

Four verifies run at a time on a machine: each waits for its turn before it builds, since six at
once have left Windows unable to start programs, each failure a dialog on the user's screen. Its
build folder is held the whole run, so a second verify of that folder is refused. --only runs
just the tests matching a regular expression, in its turn, for what a fix touches; it never prints
"everything passed", which only the whole check does.

clang-format and clang-tidy come from $CLANG_FORMAT and $CLANG_TIDY, or PATH, or where the user's
machine has them.
"""

import argparse
import hashlib
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
FALLBACKS = {
    "CLANG_FORMAT": "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin/clang-format.exe",
    "CLANG_TIDY": "C:/Users/Hunter/AppData/Local/Programs/CLion/bin/clang/win/x64/bin/clang-tidy.exe",
}
# The clang-format CI pins (ci.yml), which every check formats with.
CLANG_FORMAT_VERSION = "19.1.5"
# Tests ctest has to list, as one registered only by a second configure is skipped without a word.
REQUIRED_TESTS = ["examples.hello", "examples.arcade", "examples.arcade.idle", "examples.arcade.golden",
                  "examples.arcade.rules", "examples.arcade.usage", "fbengine.package"]
failures = []
# One lock for each verify that may run at a time on this machine.
TURNS = ["turn.lock", "turn2.lock", "turn3.lock", "turn4.lock"]


def step(name):
    print(f"\n== {name}", flush=True)


def fail(message):
    print(f"FAIL: {message}", flush=True)
    failures.append(message)


def try_lock(file):
    """Takes `file`'s first byte for this process, until it exits; if it could."""
    try:
        if os.name == "nt":
            import msvcrt

            file.seek(0)
            msvcrt.locking(file.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl

            fcntl.flock(file, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        return False
    return True


def lock_file(name):
    folder = pathlib.Path(tempfile.gettempdir()) / "fbengine-verify"
    folder.mkdir(exist_ok=True)
    # Kept open, as closing it lets go.
    return (folder / name).open("a+b")


def hold_build(build):
    """Holds the build folder for this run, or refuses: two verifies of one folder undo each other."""
    name = hashlib.sha256(str(build).lower().encode()).hexdigest()[:16]
    file = lock_file(f"build-{name}.lock")
    if not try_lock(file):
        sys.exit(f"another verify is running in {build}")
    return file


def wait_for_turn():
    """Waits for one of the machine's turns, and holds it until this verify exits."""
    files = [lock_file(name) for name in TURNS]
    waited = False
    while True:
        for file in files:
            if try_lock(file):
                return file
        if not waited:
            print("waiting for the other verifies on this machine to finish", flush=True)
            waited = True
        time.sleep(5)


def run(command, **options):
    """Runs `command`, its output shown as it goes; its exit code."""
    return subprocess.run(command, cwd=options.pop("cwd", ROOT), **options).returncode


def output(command):
    return subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False).stdout


def tool(variable, name):
    found = os.environ.get(variable) or shutil.which(name)
    if found is None and pathlib.Path(FALLBACKS[variable]).exists():
        found = FALLBACKS[variable]
    return found


def sources():
    """Every C and C++ file of ours, tracked or new, not a dependency's."""
    patterns = ["*.h", "*.hpp", "*.c", "*.cpp"]
    files = output(["git", "ls-files", *patterns]).split()
    files += output(["git", "ls-files", "--others", "--exclude-standard", *patterns]).split()
    return sorted({file for file in files if not file.startswith("build")})


def pinned_clang_format():
    """clang-format at the version CI pins, which formats as CI does; another version formats some
    lines differently, so it's never used."""
    candidates = [os.environ.get("CLANG_FORMAT"), FALLBACKS["CLANG_FORMAT"], shutil.which("clang-format")]
    for candidate in candidates:
        if candidate and pathlib.Path(candidate).exists() or (candidate and shutil.which(candidate)):
            version = output([candidate, "--version"])
            if f"version {CLANG_FORMAT_VERSION}" in version:
                return candidate
    return None


def changed_cpp(base):
    """The .cpp files to tidy: those that changed since `base`, and every .cpp of an area whose
    header changed."""
    changed = set(output(["git", "diff", "--name-only", base, "--", "*.h", "*.hpp", "*.cpp"]).split())
    changed |= set(output(["git", "ls-files", "--others", "--exclude-standard", "*.cpp"]).split())
    files = {file for file in changed if file.endswith(".cpp") and (ROOT / file).exists()}
    for header in changed:
        if header.endswith((".h", ".hpp")):
            area = pathlib.PurePosixPath(header).parent
            files |= {file for file in output(["git", "ls-files", f"{area}/*.cpp"]).split()}
    return sorted(files)


def check_build(build):
    step("build")
    if not (build / "CMakeCache.txt").exists():
        if run(["cmake", "-S", ".", "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug"]) != 0:
            fail("cmake couldn't configure")
            return False
    if run(["cmake", "--build", str(build)]) != 0:
        fail("the build failed")
        return False
    return True


def check_tests(build, only=None):
    step("ctest")
    listed = re.findall(r"Test +#\d+: (\S+)", output(["ctest", "--test-dir", str(build), "-N"]))
    for name in REQUIRED_TESTS:
        if name not in listed:
            fail(f"ctest doesn't list {name}")
    environment = dict(os.environ, SDL_VIDEO_DRIVER="offscreen")
    command = ["ctest", "--test-dir", str(build), "--output-on-failure"]
    if only is not None:
        command += ["--no-tests=error", "-R", only]
    if run(command, env=environment) != 0:
        fail("ctest failed")


def check_locks():
    step("verify's locks")
    if run([sys.executable, str(ROOT / "tools" / "verify_test.py")]) != 0:
        fail("verify's locks don't hold")


def this_target():
    """This machine's target, as FutureBlitz names it, whose DLLs the build made."""
    system = {"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux")
    machine = platform.machine().lower()
    return f"{system}-{'arm64' if machine in ('arm64', 'aarch64') else 'x64'}"


def check_exports():
    step("exports")
    # Other targets' DLLs, from CI's artifacts, are only checked on their own machines.
    dlls = []
    for native in sorted((ROOT / "native").iterdir()):
        if not native.is_dir() or native.name == "common":
            continue
        folder = ROOT / "packages" / native.name / "native" / this_target()
        found = [file for file in folder.rglob("*") if file.suffix in (".dll", ".so", ".dylib")]
        if not found:
            fail(f"{native.name} has no built DLL in {folder.relative_to(ROOT)}")
        dlls += found
    names = [dll.relative_to(ROOT).as_posix() for dll in dlls]
    if names and run(["sh", "tools/check-exports.sh", "fbe_", *names]) != 0:
        fail("a DLL exports more than its C API, or two DLLs export one name")


def check_bindings():
    step("bindings")
    if run(["sh", "tools/bindgen.sh", "--check"]) != 0:
        fail("the bindings aren't what their headers make; run tools/bindgen.sh")


def check_format():
    step(f"clang-format {CLANG_FORMAT_VERSION}")
    clang_format = pinned_clang_format()
    if clang_format is None:
        fail(f"no clang-format {CLANG_FORMAT_VERSION}, the version CI pins; set CLANG_FORMAT to one")
        return
    files = sources()
    if files and run([clang_format, "--dry-run", "-Werror", *files]) != 0:
        fail("clang-format would change files")


def check_tidy(build, base):
    step(f"clang-tidy on what changed since {base}")
    clang_tidy = tool("CLANG_TIDY", "clang-tidy")
    if clang_tidy is None:
        fail("clang-tidy isn't on PATH; set CLANG_TIDY")
        return
    files = changed_cpp(base)
    if not files:
        print("no C++ changed")
        return
    for file in files:
        result = subprocess.run([clang_tidy, "-p", str(build), "--quiet", file], cwd=ROOT,
                                capture_output=True, text=True, check=False)
        findings = [line for line in result.stdout.splitlines() if "warning:" in line or "error:" in line]
        if findings:
            print("\n".join(findings))
            fail(f"clang-tidy found {len(findings)} in {file}")
        else:
            print(f"ok   {file}")


def check_fb_format():
    step("fb fmt")
    if run(["fb", "fmt", "--check", "packages", "examples"]) != 0:
        fail("fb fmt would change files")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", default="build", help="the build folder (default: build)")
    parser.add_argument("--base", default="origin/master", help="what clang-tidy diffs against")
    parser.add_argument("--no-build", action="store_true", help="use the build as it is")
    parser.add_argument("--no-tidy", action="store_true", help="skip clang-tidy")
    parser.add_argument("--only", help="build, and run only the tests this regular expression matches")
    options = parser.parse_args()
    build = (ROOT / options.build).resolve()
    # Held until this process exits.
    held = [hold_build(build), wait_for_turn()]

    built = options.no_build or check_build(build)
    if options.only is not None:
        if built:
            check_tests(build, options.only)
        if failures:
            sys.exit(1)
        print()
        print("those tests passed; this isn't a landing's check")
        del held
        return
    check_locks()
    if built:
        check_tests(build)
        check_exports()
    check_bindings()
    check_format()
    if not options.no_tidy and built:
        check_tidy(build, options.base)
    check_fb_format()

    print()
    if failures:
        print(f"{len(failures)} failure(s):")
        for message in failures:
            print(f"  {message}")
        sys.exit(1)
    print("everything passed")
    del held


if __name__ == "__main__":
    main()
