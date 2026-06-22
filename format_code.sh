#!/bin/bash

set -e

# Prefer a recent clang-format. The repo targets C++23, and older versions
# (especially anything < 17) mangle the formatting. Search highest-first; fall
# back to whatever `clang-format` resolves to on PATH only if no suffixed
# binary is found.
CLANG_FORMAT=""
for version in 20 19 18 17; do
    if command -v "clang-format-${version}" >/dev/null 2>&1; then
        CLANG_FORMAT="clang-format-${version}"
        break
    fi
done

if [[ -z "${CLANG_FORMAT}" ]]; then
    if ! command -v clang-format >/dev/null 2>&1; then
        echo "format_code.sh: no clang-format binary found on PATH" >&2
        exit 1
    fi
    CLANG_FORMAT="clang-format"
fi

echo "format_code.sh: using $(${CLANG_FORMAT} --version)"

git add .

git ls-files '*.cpp' '*.h' | xargs "${CLANG_FORMAT}" -i
