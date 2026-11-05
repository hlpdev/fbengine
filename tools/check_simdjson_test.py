"""Checks check_simdjson.py's reading of a compile's command and of -M's rule, apart from any compiler.

    python tools/check_simdjson_test.py
"""

import importlib.util
import pathlib

TOOLS = pathlib.Path(__file__).resolve().parent


def load():
    spec = importlib.util.spec_from_file_location("check_simdjson", TOOLS / "check_simdjson.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    check = load()
    # An escaped space is in a name; a backslash and newline goes on to the next line.
    rule = "x.o: /a\\ b/simdjson.h \\\n  /c/d.h\n"
    assert check.headers_of(rule) == ["/a b/simdjson.h", "/c/d.h"], check.headers_of(rule)
    assert check.headers_of("x.o: C:/x/y.h\r\n") == ["C:/x/y.h"]
    # Windows' command lines: quotes dropped, a path's backslashes kept, an escaped quote kept.
    windows = r'C:\mingw\bin\c++.exe -I"C:\a b\inc" -DX=\"y\" -c C:\src\gltf.cpp'
    assert check.split_command(windows, True) == [
        r"C:\mingw\bin\c++.exe", r"-IC:\a b\inc", '-DX="y"', "-c", r"C:\src\gltf.cpp"], check.split_command(windows, True)
    # A POSIX shell's: quotes dropped, a backslash escaping.
    posix = "/usr/bin/c++ -I'/a b/inc' -DX=\\\"y\\\" -c /src/gltf.cpp"
    assert check.split_command(posix, False) == [
        "/usr/bin/c++", "-I/a b/inc", '-DX="y"', "-c", "/src/gltf.cpp"], check.split_command(posix, False)
    assert check.is_msvc(r"C:\VS\bin\cl.exe") and check.is_msvc("clang-cl") and not check.is_msvc("clang++")
    print("check_simdjson.py reads commands and rules as it should")


if __name__ == "__main__":
    main()
