"""Makes native/fbengine/text/default_font.inc, the bytes of Font.BuiltIn's font: a small Latin subset
of Noto Sans Regular, its hinting and every table a game's text doesn't need left out, as a C array.

    python tools/make_default_font.py <NotoSans-Regular.ttf>

Its source, under the SIL Open Font License (packages/fbengine/notices/noto-sans.txt), is Noto Sans
2.015, at
https://github.com/notofonts/notofonts.github.io/raw/28b15b4b43b7bed62b5cf6e6b0b5ff5846270535/fonts/NotoSans/hinted/ttf/NotoSans-Regular.ttf
whose SHA-256 it checks. It needs fontTools 4.68.0 (`pip install fonttools==4.68.0`), as nothing else
of the engine's does but tests/fonts/make_fonts.py; the build uses only what it wrote.
"""

import hashlib
import io
import pathlib
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE_SHA256 = "478c558ea716033cd60c03438f628dfa75694dcf6b5f6d505a2f05fd2b4f3823"

# Basic Latin, Latin-1, Latin Extended-A, the punctuation games print, the euro, the trademark, the
# arrows, and the replacement character.
RANGES = [(0x20, 0x7E), (0xA0, 0x17F), (0x2010, 0x2027), (0x2030, 0x203A), (0x20AC, 0x20AC),
          (0x2122, 0x2122), (0x2190, 0x2193), (0xFFFD, 0xFFFD)]


def main():
    source = pathlib.Path(sys.argv[1]).read_bytes()
    if hashlib.sha256(source).hexdigest() != SOURCE_SHA256:
        sys.exit(f"{sys.argv[1]} isn't Noto Sans 2.015's file")
    font = TTFont(io.BytesIO(source))
    options = subset.Options()
    options.hinting = False
    options.layout_features = ["kern", "liga", "mark", "mkmk", "ccmp", "locl"]
    options.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]
    options.notdef_outline = True
    options.recalc_timestamp = False
    subsetter = subset.Subsetter(options)
    points = [point for first, last in RANGES for point in range(first, last + 1)]
    subsetter.populate(unicodes=points)
    subsetter.subset(font)
    # The same bytes each time this runs.
    font["head"].modified = font["head"].created
    written = io.BytesIO()
    font.recalcTimestamp = False
    font.save(written)
    data = written.getvalue()
    lines = ["// Made by tools/make_default_font.py: a Latin subset of Noto Sans, under the OFL."]
    for at in range(0, len(data), 24):
        lines.append("".join(f"{byte}," for byte in data[at:at + 24]))
    target = ROOT / "native/fbengine/text/default_font.inc"
    target.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{target}: {len(data)} bytes of font, {len(points)} characters")


main()
