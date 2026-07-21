#!/usr/bin/env bash

set -euo pipefail

readonly CAT_REPOSITORY_URL="https://github.com/dianping/cat.git"

usage() {
    cat <<'EOF'
Usage: prepare_cat_source.sh [--ref REVISION] [--target PATH]

Prepare a sparse official CAT checkout containing lib/c and lib/cpp.

Options:
  --ref REVISION  Fetch and detach at a tag, branch, or commit. Existing cached
                  source is reused without network access when this is omitted.
  --target PATH   Checkout directory, relative to the repository root unless
                  absolute (default: temp/cat).
  -h, --help      Show this help.
EOF
}

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(git -C "$script_dir" rev-parse --show-toplevel 2>/dev/null) ||
    fail "the skill script is not inside a Git repository"

requested_ref=""
target_arg="temp/cat"

while (($# > 0)); do
    case "$1" in
        --ref)
            (($# >= 2)) || fail "--ref requires a value"
            requested_ref=$2
            shift 2
            ;;
        --target)
            (($# >= 2)) || fail "--target requires a value"
            target_arg=$2
            shift 2
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            fail "unknown argument: $1"
            ;;
    esac
done

if [[ "$target_arg" = /* ]]; then
    target=$target_arg
else
    target="$repo_root/$target_arg"
fi

validate_checkout() {
    local origin

    [[ -d "$target" ]] || fail "checkout path is not a directory: $target"
    git -C "$target" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
        fail "checkout path is not a Git working tree: $target"
    origin=$(git -C "$target" remote get-url origin 2>/dev/null) ||
        fail "checkout has no origin remote: $target"
    [[ "$origin" == "$CAT_REPOSITORY_URL" ]] ||
        fail "checkout origin is $origin, expected $CAT_REPOSITORY_URL"
}

if [[ -e "$target" ]]; then
    validate_checkout
    if [[ -n "$requested_ref" ]]; then
        [[ -z "$(git -C "$target" status --porcelain --untracked-files=all)" ]] ||
            fail "refusing to update dirty checkout: $target"
        git -C "$target" fetch --depth 1 origin "$requested_ref"
        git -C "$target" checkout --detach FETCH_HEAD
    fi
else
    mkdir -p "$(dirname -- "$target")"
    git clone --depth 1 --filter=blob:none --sparse "$CAT_REPOSITORY_URL" "$target"
    validate_checkout
    if [[ -n "$requested_ref" ]]; then
        git -C "$target" fetch --depth 1 origin "$requested_ref"
        git -C "$target" checkout --detach FETCH_HEAD
    fi
fi

if [[ ! -f "$target/lib/c/src/ccat/client.c" || ! -f "$target/lib/cpp/src/cppcat/client.cpp" ]]; then
    [[ -z "$(git -C "$target" status --porcelain --untracked-files=all)" ]] ||
        fail "refusing to change sparse checkout paths in dirty checkout: $target"
    git -C "$target" sparse-checkout set lib/c lib/cpp
fi

[[ -f "$target/lib/c/include/client.h" ]] || fail "missing official C public API"
[[ -f "$target/lib/c/src/ccat/client.c" ]] || fail "missing official C implementation"
[[ -f "$target/lib/cpp/include/client.hpp" ]] || fail "missing official C++ public API"
[[ -f "$target/lib/cpp/src/cppcat/client.cpp" ]] || fail "missing official C++ facade"
[[ -e "$target/lib/cpp/src/ccat" ]] || fail "C++ to C source link is unresolved"
[[ -e "$target/lib/cpp/src/lib" ]] || fail "C++ low-level library link is unresolved"

commit=$(git -C "$target" rev-parse HEAD)
printf 'CAT reference ready: %s\n' "$target"
printf 'Origin: %s\n' "$CAT_REPOSITORY_URL"
printf 'Commit: %s\n' "$commit"
printf 'Sources: %s, %s\n' "$target/lib/c" "$target/lib/cpp"
