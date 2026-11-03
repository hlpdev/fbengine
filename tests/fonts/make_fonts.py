"""The text tests' own fonts, made with fontTools: boxes, not letters, under families no OS has.

- FbeTestText-Regular.ttf: "Fbe Test Text", outlines for a, b, U+231A (a watch, which Unicode
  shows as an emoji), and U+E903, and none for U+0332, the combining low line.
- FbeTestColor-Regular.ttf: "Fbe Test Color", COLR layers, red over blue, for U+231A and U+E903,
  and an outline for a.
- FbeTestMarks-Regular.ttf: "Fbe Test Marks", outlines for a and U+0332.
- FbeTestSvg-Regular.ttf: "Fbe Test Svg", an SVG picture of U+E903, which FreeType isn't built to
  draw here, over an outline of it, and an SVG picture of U+E904 with an empty outline.
- FbeTestHuge-Regular.ttf: "Fbe Test Huge", an outline for H 15 ems across and tall, as a
  hostile font's glyph can be (FreeType renders up to 16), and for a, a hair over half an em
  across.
- FbeTestSbix-Regular.ttf: "Fbe Test Sbix", an sbix PNG of a at 32 pixels an em, which FreeType
  isn't built to read here, over an outline of it.

Run it again with `python tests/fonts/make_fonts.py` if they change. It needs fontTools 4.68.0
(`pip install fonttools==4.68.0`); the tests and the build use only the fonts it wrote.
"""

import pathlib
import struct
import zlib

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import newTable
from fontTools.ttLib.tables.S_V_G_ import SVGDocument
from fontTools.ttLib.tables.sbixGlyph import Glyph as SbixGlyph
from fontTools.ttLib.tables.sbixStrike import Strike

HERE = pathlib.Path(__file__).parent


def box(x0, y0, x1, y1):
    pen = TTGlyphPen(None)
    pen.moveTo((x0, y0))
    pen.lineTo((x0, y1))
    pen.lineTo((x1, y1))
    pen.lineTo((x1, y0))
    pen.closePath()
    return pen.glyph()


def empty():
    return TTGlyphPen(None).glyph()


def png():
    """A red pixel, as a PNG."""
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (bytes([0x89]) + b"PNG" + bytes([13, 10, 26, 10]) +
            chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes([0, 255, 0, 0, 255]))) + chunk(b"IEND", b""))


def made(family, glyphs, cmap, file, colors=None, svg=None, sbix=None):
    """A font of `glyphs`, names to (advance, outline), mapped by `cmap`, with COLR's layers, SVG
    documents, (document, first glyph, last glyph), and sbix's PNGs, by size, then glyph."""
    builder = FontBuilder(1000, isTTF=True)
    order = [".notdef"] + [name for name in glyphs if name != ".notdef"]
    builder.setupGlyphOrder(order)
    builder.setupCharacterMap(cmap)
    outlines = {".notdef": box(50, 0, 450, 700)}
    advances = {".notdef": (500, 50)}
    for name, (advance, outline) in glyphs.items():
        outlines[name] = outline
        advances[name] = (advance, 0)
    builder.setupGlyf(outlines)
    builder.setupHorizontalMetrics(advances)
    builder.setupHorizontalHeader(ascent=800, descent=-200)
    builder.setupNameTable({"familyName": family, "styleName": "Regular"})
    builder.setupOS2(sTypoAscender=800, sTypoDescender=-200, sTypoLineGap=0, usWinAscent=800,
                     usWinDescent=200, usWeightClass=400)
    builder.setupPost()
    if colors is not None:
        layers, palette = colors
        builder.setupCOLR(layers)
        builder.setupCPAL([palette])
    if svg is not None:
        table = newTable("SVG ")
        table.docList = [SVGDocument(document, first, last, False) for document, first, last in svg]
        builder.font["SVG "] = table
    if sbix is not None:
        table = newTable("sbix")
        table.version = 1
        table.flags = 1
        table.strikes = {}
        for ppem, pictures in sbix.items():
            strike = Strike(ppem=ppem, resolution=72)
            for name, data in pictures.items():
                strike.glyphs[name] = SbixGlyph(glyphName=name, graphicType="png ", imageData=data)
            table.strikes[ppem] = strike
        builder.font["sbix"] = table
    builder.save(str(HERE / file))


made("Fbe Test Text",
     {"space": (300, empty()), "a": (500, box(50, 0, 450, 500)), "b": (600, box(50, 0, 550, 700)),
      "watch": (800, box(100, 0, 700, 700)), "private": (800, box(100, 0, 700, 600))},
     {0x20: "space", 0x61: "a", 0x62: "b", 0x231A: "watch", 0xE903: "private"},
     "FbeTestText-Regular.ttf")

made("Fbe Test Color",
     {"a": (500, box(50, 0, 450, 500)), "watch": (900, box(0, 0, 900, 800)),
      "private": (900, box(0, 0, 900, 800)), "red": (900, box(0, 0, 900, 400)),
      "blue": (900, box(0, 400, 900, 800))},
     {0x61: "a", 0x231A: "watch", 0xE903: "private"},
     "FbeTestColor-Regular.ttf",
     ({"watch": [("red", 0), ("blue", 1)], "private": [("red", 0), ("blue", 1)]},
      [(1.0, 0.0, 0.0, 1.0), (0.0, 0.0, 1.0, 1.0)]))

made("Fbe Test Marks",
     {"a": (520, box(50, 0, 470, 500)), "lowline": (0, box(-450, -150, -50, -100))},
     {0x61: "a", 0x0332: "lowline"},
     "FbeTestMarks-Regular.ttf")

picture = ('<svg xmlns="http://www.w3.org/2000/svg" id="glyph{}">'
           '<rect x="100" y="-700" width="600" height="700" fill="red"/></svg>')
made("Fbe Test Svg",
     {"private": (800, box(100, 0, 700, 700)), "blank": (800, empty())},
     {0xE903: "private", 0xE904: "blank"},
     "FbeTestSvg-Regular.ttf",
     svg=[(picture.format(1), 1, 1), (picture.format(2), 2, 2)])

made("Fbe Test Huge",
     {"H": (15000, box(0, -1000, 15000, 14000)), "a": (501, box(50, 0, 450, 500))},
     {0x48: "H", 0x61: "a"},
     "FbeTestHuge-Regular.ttf")

made("Fbe Test Sbix",
     {"a": (500, box(50, 0, 450, 500))},
     {0x61: "a"},
     "FbeTestSbix-Regular.ttf",
     sbix={32: {"a": png()}})
