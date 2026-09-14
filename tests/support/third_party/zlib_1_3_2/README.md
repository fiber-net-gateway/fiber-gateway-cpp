# zlib 1.3.2 test reference

This directory holds unmodified source files from the official zlib 1.3.2
release archive. They are compiled into a test-only CMake OBJECT target and
linked into `fiber_tests` and `lite_nginx_tests` exclusively, giving the test
suite an independent DEFLATE/gzip decoder (and a fixed-parameter compressor
for differential and baseline checks) without any dependency on a system
zlib, Python, or the `gzip` command.

The files here are **not** production code:

- They never enter `fiber_lib`, `fiber_http_compression`, or any application
  target, and the directory is never placed on a production include path.
- Every externally visible symbol is renamed to `fiber_test_zlib_*` at compile
  time via the force-included `fiber_test_zlib_prefix.h`, so production code
  cannot link against these objects even by accident.
- The upstream algorithm is kept byte-for-byte: no local edits to these files.
  `manifest.sha256` records the archive hash and per-file hashes.

## Build configuration

- Compiled as C with the archive's default configuration: static CRC and
  fixed-inflate tables from `crc32.h`/`inffixed.h` (no `DYNAMIC_CRC_TABLE`),
  no `ZLIB_DEBUG`, no architecture-specific contrib hooks.
- Only the symbol-prefix header is force-included; no other defines or source
  modifications.
- The upstream CMakeLists is not used; the object target is declared in
  `cmake/FiberZlibReference.cmake`.

## Updating

To move to a newer zlib release: download the official archive, verify its
SHA-256, refresh the files here from the archive root, regenerate
`manifest.sha256`, and re-verify that `nm` on the built objects shows only
`fiber_test_zlib_*` global symbols.
