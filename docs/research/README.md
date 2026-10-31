# Research

Read-throughs of Blitz3D's source (zlib) and SCP: Containment Breach's (CC BY-SA 3.0), made on
2026-10-31 to plan the engine. They describe behavior and numbers, not code.

- blitz3d-2d.md, blitz3d-3d.md, blitz3d-input-audio-net.md, blitz3d-language-std.md: every
  Blitz3D command, what it does, exactly, and where its job lands.
- scpcb-requirements.md: what SCP:CB uses, its formats (.rmesh in full), its systems, and the
  numbers behind its feel, with file and line.

fbengine is its own engine, not a modern copy of Blitz3D's (FutureBlitz's docs/decisions.md,
2026-10-31). These files check that its features cover what SCP:CB needs (E14's samples), and
are reference for the remake, a separate, later project with its own converters. Their API
proposals, compatibility shims, and migrator notes are an agent's first pass, superseded by
docs/plan.md and each milestone's design.
