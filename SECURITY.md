# Security

## Reporting a vulnerability

Please don't report a vulnerability in a public issue. Report it privately, on GitHub, at
[Report a vulnerability](https://github.com/hlpdev/fbengine/security/advisories/new) (the
repository's Security tab, then "Report a vulnerability"). Only you and the maintainers see it.

Say what's affected and in which version of the packages, how to reproduce it, ideally with a
small program, and what someone could do with it. You'll hear back within a week, and as the fix
goes on.

## What's covered

fbengine's packages and their DLLs: above all what reads files a game didn't make itself, as
images, models, sounds, fonts, and pak files, and the asset cooker. A vulnerability in
FutureBlitz itself, fb, std, or the package registry, goes to
[FutureBlitz's](https://github.com/hlpdev/FutureBlitz/security/advisories/new).

## Supported versions

The latest release gets security fixes, as a new release of the packages.

## What happens

The maintainers confirm the report, fix it in private, and release the fix, then publish a GitHub
security advisory with a CVE, crediting you unless you'd rather not be. Please keep it private
until the advisory is out.
