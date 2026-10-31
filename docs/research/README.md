# Research

Read-throughs of Blitz3D's source (zlib) and SCP: Containment Breach's (CC BY-SA 3.0), made on
2026-10-31 to plan the engine. They describe behavior and numbers, not code.

- blitz3d-2d.md, blitz3d-3d.md, blitz3d-input-audio-net.md, blitz3d-language-std.md: every
  Blitz3D command, what it does, exactly, and where its job lands.
- scpcb-requirements.md: what SCP:CB uses, its formats (.rmesh in full), its systems, and the
  numbers behind its feel, with file and line.

fbengine is a modern engine and copies none of Blitz3D's quirks (FutureBlitz's
docs/decisions.md, 2026-10-31). These files are for converting SCP:CB's assets and writing the
remake at E14, where its quirks are reproduced in game code or fixed in its converted assets. The
API proposals in them are an agent's first pass, superseded by docs/plan.md and each milestone's
design.
