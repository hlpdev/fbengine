"""Checks verify.py's locks: a second process can't take one that's held, and can once it's let go."""

import importlib.util
import os
import pathlib
import subprocess
import sys
import textwrap

TOOLS = pathlib.Path(__file__).resolve().parent


def load():
    spec = importlib.util.spec_from_file_location("verify", TOOLS / "verify.py")
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


def check_no_error_dialogs(verify):
    """What verify starts inherits its error mode, through what starts it in turn, as cmake's libuv."""
    if os.name != "nt":
        return
    verify.no_error_dialogs()
    code = "import ctypes; print(ctypes.windll.kernel32.GetErrorMode())"
    for command in ([sys.executable, "-c", code], ["cmake", "-E", "env", sys.executable, "-c", code]):
        done = subprocess.run(command, capture_output=True, text=True, check=True)
        assert int(done.stdout) & 0x8003 == 0x8003, f"{command[0]} started a program that can show a dialog"


def main():
    verify = load()
    name = "verify-test.lock"
    held = verify.lock_file(name)
    assert verify.try_lock(held), "nothing else holds it"
    assert not other_process_takes(name), "a held lock was taken"
    held.close()
    assert other_process_takes(name), "a lock let go of couldn't be taken"
    assert len(set(verify.TURNS)) == 4, "four verifies at a time take four locks"
    check_no_error_dialogs(verify)
    print("verify's locks hold")


if __name__ == "__main__":
    main()
