#!/bin/sh
# Makes each package's bindings again from its C API: native/<package>/<area>/fbe_<area>.h (or
# another prefix's) to packages/<package>/c/fbe_<area>.fb, named as the header is so it never
# shares a module name with the package's own files or std's, and removes bindings whose header
# is gone.
# `bindgen.sh --check` changes nothing, and fails if any binding isn't what its header makes.
set -eu
cd "$(dirname "$0")/.."
check=false
if [ "${1:-}" = "--check" ]; then
    check=true
fi
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# Whether two binding files say the same, leaving out their first lines, which name the target
# fb-bindgen ran for and so differ by machine.
same_bindings() {
    [ -e "$2" ] || return 1
    tail -n +2 "$1" > "$scratch/made"
    tail -n +2 "$2" > "$scratch/kept"
    cmp -s "$scratch/made" "$scratch/kept"
}

stale=0
for native in native/*/; do
    package="$(basename "$native")"
    lib="${package}_native"
    bindings="packages/$package/c"
    mkdir -p "$scratch/$package"
    for header in "$native"*/*.h; do
        [ -e "$header" ] || continue
        name="$(basename "$header" .h)"
        fb-bindgen "$header" -o "$scratch/$package/$name.fb" --lib "$lib" 2> "$scratch/log" ||
            { cat "$scratch/log" >&2; exit 1; }
    done
    for made in "$scratch/$package"/*.fb; do
        [ -e "$made" ] || continue
        target="$bindings/$(basename "$made")"
        if ! same_bindings "$made" "$target"; then
            stale=1
            if $check; then
                echo "$target isn't what its header makes; run tools/bindgen.sh" >&2
            else
                mkdir -p "$bindings"
                cp "$made" "$target"
                echo "wrote $target"
            fi
        fi
    done
    for kept in "$bindings"/*.fb; do
        [ -e "$kept" ] || continue
        if [ ! -e "$scratch/$package/$(basename "$kept")" ]; then
            stale=1
            if $check; then
                echo "$kept has no header any more; run tools/bindgen.sh" >&2
            else
                rm "$kept"
                echo "removed $kept"
            fi
        fi
    done
done
if $check && [ "$stale" = 1 ]; then
    exit 1
fi
