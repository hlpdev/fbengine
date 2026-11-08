"""Tests check_publish.py's check of publish.yml's list, which needs no fb, on repositories of its own
made in a temporary folder: one whose list is whole, then each way it can be wrong.

    python tools/check_publish_test.py
"""

import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import check_publish  # noqa: E402

PACKAGES = {"fbengine": "library", "fbengine_ui": "library", "fbengine_video": "library",
            "fbengine_editor": "executable"}


def made(root, listed):
    for name, kind in PACKAGES.items():
        folder = root / "packages" / name
        folder.mkdir(parents=True)
        (folder / "project.toml").write_text(f'[project]\nname = "{name}"\nversion = "1.0.0"\ntype = "{kind}"\n',
                                             encoding="utf-8")
    workflow = root / ".github" / "workflows"
    workflow.mkdir(parents=True)
    words = listed.split()
    (workflow / "publish.yml").write_text(
        "      - name: Publish\n        run: |\n"
        f"          for package in {' '.join(words[:2])} \\\n                         {' '.join(words[2:])}; do\n"
        '            fb publish "packages/$package" --registry "$INDEX"\n          done\n', encoding="utf-8")


def check(name, listed, expected):
    with tempfile.TemporaryDirectory() as folder:
        root = pathlib.Path(folder)
        made(root, listed)
        wrong = check_publish.listed(root)
    ok = expected in (wrong or "") if expected else wrong is None
    print(f"{name}: {'ok' if ok else 'WRONG'}{'' if ok else ': ' + repr(wrong)}")
    return ok


def main():
    results = [
        check("whole", "fbengine fbengine_ui fbengine_video", None),
        check("a library left out", "fbengine fbengine_ui", "doesn't publish fbengine_video"),
        check("a program in it", "fbengine fbengine_ui fbengine_video fbengine_editor",
              "publishes fbengine_editor, which isn't a library"),
        check("a package that isn't there", "fbengine fbengine_ui fbengine_video fbengine_net",
              "publishes fbengine_net, which isn't a library"),
        check("a package twice", "fbengine fbengine_ui fbengine_video fbengine", "publishes a package twice"),
    ]
    if not all(results):
        return 1
    print(f"all {len(results)} cases hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
