"""Checks verify.py's locks: a second process can't take one that's held, and can once it's let go;
and what show.py finds to play."""

import importlib.util
import os
import pathlib
import subprocess
import sys
import tempfile
import textwrap

TOOLS = pathlib.Path(__file__).resolve().parent


def load(name="verify"):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def other_process_takes(name):
    code = textwrap.dedent(f"""
        import importlib.util
        spec = importlib.util.spec_from_file_location("verify", {str(TOOLS / "verify.py")!r})
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        print(module.try_lock(module.lock_file({name!r})))
    """)
    done = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, check=True)
    return done.stdout.strip() == "True"


def show_finds_what_has_a_watch():
    show = load("show")
    with tempfile.TemporaryDirectory() as folder:
        show.ROOT = pathlib.Path(folder)
        for name, text in [("b", 'x.Contains("--watch")'), ("a/deep", 'If y.Contains("--watch")'), ("c", "Print(1)")]:
            (show.ROOT / "examples" / name).mkdir(parents=True)
            (show.ROOT / "examples" / name / "main.fb").write_text(text, encoding="utf-8")
        (show.ROOT / "examples" / "b" / "other.fb").write_text('"--watch"', encoding="utf-8")
        assert show.watched() == ["a/deep", "b"], show.watched()
    done = subprocess.run([sys.executable, str(TOOLS / "show.py"), "--list"], capture_output=True, text=True, check=True)
    listed = done.stdout.split()
    assert "menu" in listed and "proof/room_files" in listed, listed
    assert "hello" not in listed, "an example with no --watch was listed"
    assert listed[-1] == "../packages/fbengine_editor", listed
    print("show.py finds what has a --watch")


def check_no_error_dialogs(verify):
    """What verify starts inherits its error mode, through what starts it in turn, as cmake's libuv."""
    if os.name != "nt":
        return
    verify.no_error_dialogs()
    code = "import ctypes; print(ctypes.windll.kernel32.GetErrorMode())"
    for command in ([sys.executable, "-c", code], ["cmake", "-E", "env", sys.executable, "-c", code]):
        done = subprocess.run(command, capture_output=True, text=True, check=True)
        assert int(done.stdout) & 0x8003 == 0x8003, f"{command[0]} started a program that can show a dialog"


def check_vulkan_driver(verify):
    """What CMake wrote is what verify gives the tests, and a build folder without it gives none, for
    check_tests to refuse."""
    with tempfile.TemporaryDirectory() as folder:
        assert verify.vulkan_driver(folder) == "", "a folder CMake hasn't written has no driver"
        (pathlib.Path(folder) / "tests-vulkan-driver.txt").write_text("lavapipe.json")
        assert verify.vulkan_driver(folder) == "lavapipe.json", "verify doesn't read CMake's driver"


def check_device_entries(verify):
    """A call of Vulkan 1.3's own entry is found anywhere in native code but device.cpp, and not in a
    comment or where it's only named."""
    with tempfile.TemporaryDirectory() as folder:
        root = pathlib.Path(folder)
        (root / "native" / "a" / "gpu").mkdir(parents=True)
        (root / "native" / "a" / "x.cpp").write_text(
            "void f() {\n    // vkCmdBeginRendering(commands, &info);\n    vkCmdEndRendering (commands);\n}\n")
        (root / "native" / "a" / "y.hpp").write_text("auto g = vkCmdBeginRenderingKHR(commands, &info);\n")
        (root / "native" / "a" / "gpu" / "device.cpp").write_text(
            "begin = core13 ? vkCmdBeginRendering : vkCmdBeginRenderingKHR;\nvkCmdBeginRendering(c, &i);\n")
        (root / "native" / "a" / "z.cpp").write_text("gpu.begin_rendering(commands, &info);\n")
        found = verify.direct_entries(root)
        assert found == ["native/a/x.cpp:3", "native/a/y.hpp:1"], found
    assert verify.direct_entries(verify.ROOT) == [], verify.direct_entries(verify.ROOT)


def check_fb_versions(verify):
    """The fb the tests run is CI's, by its version, and one that isn't is refused, saying which."""
    assert verify.required_fb("env:\n  FB_VERSION: 1.2.3\n") == "1.2.3"
    assert verify.required_fb("env:\n  OTHER: 1\n") is None
    assert verify.fb_version_of("fb 1.2.3 (c77a32a, windows-x64)\n") == "1.2.3"
    assert verify.fb_version_of("") is None
    assert verify.fb_problem("fb", "1.2.3", "1.2.3") is None
    older = verify.fb_problem("the build's fb", "1.2.2", "1.2.3")
    assert older is not None and "1.2.2" in older and "1.2.3" in older, older
    assert verify.fb_problem("fb", None, "1.2.3") is not None
    assert verify.fb_problem("fb", "1.2.3", None) is not None
    with tempfile.TemporaryDirectory() as folder:
        assert verify.cached_fb(folder) is None, "a folder CMake hasn't written has no fb"
        (pathlib.Path(folder) / "CMakeCache.txt").write_text(
            "FBENGINE_FB_TESTS:BOOL=ON\nFBENGINE_FB:FILEPATH=C:/fb-1.2.2/bin/fb.exe\n")
        assert verify.cached_fb(folder) == "C:/fb-1.2.2/bin/fb.exe", verify.cached_fb(folder)
    ci = (TOOLS.parent / ".github" / "workflows" / "ci.yml").read_text()
    assert verify.required_fb(ci) is not None, "ci.yml says no FB_VERSION this can find"
    # A cached fb that's been taken off the disk is said, not run.
    with tempfile.TemporaryDirectory() as folder:
        gone = pathlib.Path(folder) / "fb-gone" / "fb.exe"
        (pathlib.Path(folder) / "CMakeCache.txt").write_text(f"FBENGINE_FB:FILEPATH={gone.as_posix()}\n")
        said = [problem for problem in verify.fb_problems(folder, "1.2.3") if problem.startswith("the build's")]
        assert len(said) == 1 and "isn't there" in said[0] and "-DFBENGINE_FB" in said[0], said


def main():
    show_finds_what_has_a_watch()
    verify = load()
    name = "verify-test.lock"
    held = verify.lock_file(name)
    assert verify.try_lock(held), "nothing else holds it"
    assert not other_process_takes(name), "a held lock was taken"
    held.close()
    assert other_process_takes(name), "a lock let go of couldn't be taken"
    assert len(set(verify.TURNS)) == 4, "four verifies at a time take four locks"
    check_no_error_dialogs(verify)
    check_vulkan_driver(verify)
    check_device_entries(verify)
    check_fb_versions(verify)
    print("verify's locks hold")


if __name__ == "__main__":
    main()
