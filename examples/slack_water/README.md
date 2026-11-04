# Slack Water

fbengine's showcase game, for E14. [docs/slack-water.md](../../docs/slack-water.md) has its design.
WASD and the mouse, or a gamepad, walk the island; E takes, fits, and opens the sluice; Q hands things
to Pip and takes them back; Escape ends it.

## Its assets

Everything's made by code: `fb run tools -- assets`, from this folder, writes `assets/`, which are kept
in the repository, and `examples.slack_water.assets_match` has them be what the tools make.

## Running it

`fb run` reads `assets/` as they are, and reloads what changes. `fb run --release` cooks them into a
pak, and reads that.

## Shipping it

`fb build --release -o <folder>/slack_water` builds its executable, with the DLLs it needs beside it, and
cooks `build/slack_water.pak`. What a player gets is that folder, with the pak copied into it:

```
slack_water.exe
slack_water.pak
fbengine_native.dll, and the rest of the DLLs
```

On macOS, in a bundle, the executable and DLLs are in `Contents/MacOS`, and the pak in
`Contents/Resources`. The executable reads only the pak beside it, or in Resources, and refuses one of
another version: copy the pak each time it's built. With no pak, or a bad one, it says so, and exits
1, with no window. `examples.slack_water.shipped` and those after it build and run all of this.

Its other arguments are for its tests, which set `SLACK_WATER_CHECKS=1`; main.fb's first comment has
them. All but `--cooked` do nothing without it.
