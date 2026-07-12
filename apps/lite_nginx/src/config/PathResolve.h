#ifndef FIBER_LITE_NGINX_CONFIG_PATH_RESOLVE_H
#define FIBER_LITE_NGINX_CONFIG_PATH_RESOLVE_H

#include <string>
#include <string_view>

namespace fiber::lite_nginx::config {

// Resolves a path referenced by a config directive against the file that contains it.
//
// `base_source` is the source_name of the directive's file (the path the Document was
// parsed with). When `path` is absolute, it is returned lexically normalized. When
// relative, it is joined with the directory of `base_source` and lexically normalized
// (".", ".." collapsed). When `base_source` has no directory (a bare name or empty,
// e.g. inline test configs), `path` is returned unchanged (lexically normalized) so the
// historical pwd-relative behavior is preserved for those callers.
//
// Purely lexical: no filesystem access, no existence check. Existence is reported at
// the consumer (file open / TLS load), where a clear "not found" error already exists.
// Never throws; falls back to the composed raw path on any std::exception.
std::string resolve_config_path(std::string_view base_source, std::string_view path);

// Returns an absolute, symlink-resolved form of `path` for use as the top-level config
// anchor and the include cycle-detection key. Uses weakly_canonical (resolves symlinks
// for the existing leading components; does not require the whole path to exist). On
// any std::filesystem error falls back to absolute().lexically_normal(), and finally to
// the raw input. Never throws.
std::string canonicalize_path(std::string_view path);

} // namespace fiber::lite_nginx::config

#endif // FIBER_LITE_NGINX_CONFIG_PATH_RESOLVE_H
