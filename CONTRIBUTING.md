# Contributing

Thanks for helping with fbengine. Bug reports, ideas, and pull requests are all welcome. Everyone
in the project's spaces keeps to the [code of conduct](CODE_OF_CONDUCT.md).

## Issues

[Open an issue](https://github.com/hlpdev/fbengine/issues/new/choose) for a bug in a package or
an idea for one. Look for an existing one first, and for a bug, give the smallest program that
shows it, the packages' version, and what `fb --version` prints. A bug in the language, fb, or
std goes to [FutureBlitz's issues](https://github.com/hlpdev/FutureBlitz/issues/new/choose), and a
vulnerability is reported privately, as [SECURITY.md](SECURITY.md) says.

A new issue gets `needs-triage`. A maintainer gives it its package (`area/fbengine`,
`area/audio`, and the others), then marks it `confirmed`, asks for a program that shows it with
`needs-repro`, or closes it as a `duplicate` or `wontfix`. `good first issue` marks a small one
that's a good start, and `help wanted` one that's open to anyone who'd like to take it.

## Pull requests

Any pull request is welcome. For a new feature, or a change to a package's API, opening an issue
first lets it be talked through before you write it; [docs/architecture.md](docs/architecture.md)
has how the engine fits together.

A pull request:

- Builds and passes its tests on Windows, Linux, and macOS, as CI checks.
- Comes with a test for what it fixes or adds. Tests never open a window: they run on SDL's
  offscreen driver and a software Vulkan.
- Follows the style: FutureBlitz's `.clang-format` and `.clang-tidy`, copied here, for C and C++,
  and `fb fmt` for .fb. A package's DLL exports only its C API, `fbe_<area>_<verb>` functions.
- Keeps to one change, so it's easy to review.

Commit messages are short and lowercase, saying what changed, with the area first when it helps:
`audio: a sound's pan`.

## Building and testing

[Building](README.md#building) has what fbengine needs. `tools/verify.py` runs every check, as a
landing does: the build, ctest, the export check, the bindings, clang-format, clang-tidy on what
changed, and `fb fmt --check`:

```sh
python tools/verify.py
```

## License

fbengine is under the [Apache License 2.0](LICENSE), and what you contribute is under it too, as
its section 5 says.
