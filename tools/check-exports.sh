#!/bin/sh
# Checks each package's DLL exports its C API and nothing else, like the static libraries in it,
# and that no two DLLs export one name: every package's DLL loads into one program, where a name
# two of them export would resolve to whichever loaded first.
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
done

shared="$(sort "$all" | uniq -d)"
if [ -n "$shared" ]; then
    echo "more than one DLL exports these names:" >&2
    printf '%s\n' "$shared" >&2
    exit 1
fi
