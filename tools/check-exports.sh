#!/bin/sh
# Checks a package's DLL exports its C API and nothing else, like the static libraries in it:
# check-exports.sh <dll, .so, or .dylib> <prefix, like fbe_>.
set -eu
file="$1"
prefix="$2"

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
echo "$file exports $(printf '%s\n' "$names" | grep -c "^$prefix") $prefix functions, and nothing else"
