#!/bin/sh
# Checks each package's DLL exports its C API and nothing else, like the static libraries in it,
# and that no two DLLs export one name: every package's DLL loads into one program, where a name
# two of them export would resolve to whichever loaded first. And that each loads only the system's
# own libraries, as a player's machine has nothing else: no Homebrew's, nor MinGW's runtime.
#     check-exports.sh <prefix, like fbe_> <dll, .so, or .dylib>...
set -eu
prefix="$1"
shift
all="$(mktemp)"
trap 'rm -f "$all"' EXIT

for file in "$@"; do
    case "$file" in
        *.dll) names="$(objdump -p "$file" | sed -n '/\[Ordinal\/Name Pointer\] Table/,/^$/p' |
                        grep '+base' | awk '{ print $NF }')" ;;
        *.dylib) names="$(nm -gU "$file" | awk '{ print $3 }' | sed 's/^_//')" ;;
        *) names="$(nm -D --defined-only "$file" | awk '{ print $3 }' | sed 's/@.*//')" ;;
    esac

    if [ -z "$(printf '%s\n' "$names" | grep "^$prefix")" ]; then
        echo "$file exports none of its $prefix functions" >&2
        exit 1
    fi
    others="$(printf '%s\n' "$names" | grep -v "^$prefix" | grep -v '^$' || true)"
    if [ -n "$others" ]; then
        echo "$file exports more than its $prefix functions:" >&2
        printf '%s\n' "$others" | head -n 20 >&2
        exit 1
    fi
    printf '%s\n' "$names" | grep "^$prefix" | sort -u >> "$all"
    echo "$file exports $(printf '%s\n' "$names" | grep -c "^$prefix") $prefix functions, and nothing else"

    # What it loads that isn't the system's: on macOS, outside /usr/lib and /System; on Linux, other
    # than glibc's own; on Windows, other than the API sets and what's in System32.
    case "$file" in
        *.dll)
            system="${SYSTEMROOT:-C:/Windows}"
            if command -v cygpath > /dev/null 2>&1; then system="$(cygpath -u "$system")"; fi
            foreign=""
            for name in $(objdump -p "$file" | awk '/DLL Name:/ { print $3 }'); do
                case "$name" in
                    api-ms-win-*) ;;
                    *) [ -f "$system/System32/$name" ] || foreign="$foreign $name" ;;
                esac
            done ;;
        *.dylib)
            foreign="$(otool -L "$file" | tail -n +2 | awk '{ print $1 }' |
                       grep -v -e '^/usr/lib/' -e '^/System/Library/' || true)" ;;
        *)
            foreign="$(readelf -d "$file" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' |
                       grep -v -E '^(libc|libm|libdl|libpthread|librt)\.so\.[0-9]+$' |
                       grep -v -E '^ld-linux[-a-z0-9_.]*\.so\.[0-9]+$' || true)" ;;
    esac
    if [ -n "$(printf '%s' "$foreign" | tr -d ' ')" ]; then
        echo "$file loads what isn't the system's:" $foreign >&2
        exit 1
    fi
    echo "$file loads only the system's libraries"
done

shared="$(sort "$all" | uniq -d)"
if [ -n "$shared" ]; then
    echo "more than one DLL exports these names:" >&2
    printf '%s\n' "$shared" >&2
    exit 1
fi
