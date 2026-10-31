# Research

Read-throughs of Blitz3D's source (zlib) and SCP: Containment Breach's (CC BY-SA 3.0), made on
2026-10-31 to plan the engine. They describe behavior and numbers, not code.

- blitz3d-2d.md, blitz3d-3d.md, blitz3d-input-audio-net.md, blitz3d-language-std.md: every
  Blitz3D command, what it does, exactly, and where its job lands.
- scpcb-requirements.md: what SCP:CB 1.3.11, the original, uses, its formats (.rmesh in full),
  its systems, and the numbers behind its feel, with file and line. The remake's target is
  Ultimate Edition v5 (the same game as SCP: Containment Breach Remastered).
- ue5-additions.md: what Ultimate Edition v5.5.4.1 needs from an engine beyond 1.3.11, which is
  little: it's a content mod on the same features. A vertical field of view, a closer near plane,
  a 3D main menu, screenshots, a third video, bump maps on most rooms, more overlays; every item
  is mapped to docs/plan.md's steps.

fbengine is its own engine, not a modern copy of Blitz3D's (FutureBlitz's docs/decisions.md,
2026-10-31). These files check that its features cover what SCP:CB needs (E14's samples), and
are reference for the remake, a separate, later project with its own converters. Their API
proposals, compatibility shims, and migrator notes are an agent's first pass, superseded by
docs/plan.md and each milestone's design.
