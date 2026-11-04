"""Checks verify.py's locks: a second process can't take one that's held, and can once it's let go;
and what show.py finds to play."""

import importlib.util
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
    print("verify's locks hold")


if __name__ == "__main__":
    main()
