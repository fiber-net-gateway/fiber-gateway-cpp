#!/usr/bin/env bash
#
# Prepare the pinned zlib reference sources under temp/zlib-reference/1.3.2.
#
# Production code does not link zlib anymore; the full upstream sources are
# only needed by tools such as scripts/build_nginx.sh (--with-zlib) and are
# cached here so every tool uses the same verified version.
#
# Usage:
#   scripts/prepare_zlib_reference.sh
#
# The archive is downloaded to a temporary file and its SHA-256 is verified
# before anything is committed to disk. An existing cache is validated
# (version string plus required files) and reused without downloading.
#
set -euo pipefail

zlib_version="1.3.2"
zlib_url="https://zlib.net/zlib-${zlib_version}.tar.gz"
zlib_sha256="bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/.." && pwd)"
ref_root="$project_root/temp/zlib-reference"
source_dir="$ref_root/${zlib_version}"
archive_path="$ref_root/zlib-${zlib_version}.tar.gz"
extract_root="$ref_root/.extract"

fail() {
    echo "error: $*" >&2
    exit 1
}

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

verify_archive() {
    [[ "$(sha256_of "$1")" == "$zlib_sha256" ]]
}

cached_sources_valid() {
    [[ -f "$source_dir/zlib.h" && -x "$source_dir/configure" ]] || return 1
    grep -q "#define ZLIB_VERSION \"${zlib_version}\"" "$source_dir/zlib.h"
}

download_archive() {
    local partial_path="$archive_path.part"

    echo "Downloading $zlib_url"
    if command -v curl >/dev/null 2>&1; then
        curl --fail --location --retry 3 --output "$partial_path" "$zlib_url"
    elif command -v wget >/dev/null 2>&1; then
        wget --output-document="$partial_path" "$zlib_url"
    else
        fail "curl or wget is required to download zlib"
    fi

    verify_archive "$partial_path" || fail "SHA-256 mismatch for the downloaded zlib archive"
    mv "$partial_path" "$archive_path"
}

command -v tar >/dev/null 2>&1 || fail "tar is required to extract zlib"
sha256_of /dev/null >/dev/null 2>&1 || fail "sha256sum or shasum is required to verify the zlib archive"

if cached_sources_valid; then
    echo "Using cached zlib ${zlib_version} reference sources in $source_dir"
    exit 0
fi

mkdir -p "$ref_root"
rm -rf "$extract_root"

if [[ -f "$archive_path" ]]; then
    if verify_archive "$archive_path"; then
        echo "Using cached archive $archive_path"
    else
        echo "Discarding cached archive $archive_path (SHA-256 mismatch)"
        rm -f "$archive_path"
        download_archive
    fi
else
    download_archive
fi

echo "Extracting zlib sources to $source_dir"
mkdir -p "$extract_root"
tar -xzf "$archive_path" -C "$extract_root"
[[ -f "$extract_root/zlib-${zlib_version}/zlib.h" ]] ||
    fail "archive did not contain a zlib-${zlib_version}/ top-level directory"
rm -rf "$source_dir"
mv "$extract_root/zlib-${zlib_version}" "$source_dir"
rm -rf "$extract_root"

cached_sources_valid || fail "extracted zlib sources failed validation"
echo "zlib ${zlib_version} reference sources are ready in $source_dir"
