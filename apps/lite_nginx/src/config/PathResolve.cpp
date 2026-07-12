#include "PathResolve.h"

#include <filesystem>

namespace fiber::lite_nginx::config {

std::string resolve_config_path(std::string_view base_source, std::string_view path) {
    namespace fs = std::filesystem;
    try {
        fs::path target(path);
        if (!target.is_absolute() && !base_source.empty()) {
            target = fs::path(base_source).parent_path() / target;
        }
        return target.lexically_normal().string();
    } catch (const std::exception &) {
        // Best-effort fallback: compose the raw strings. Lexical normalization is
        // advisory; consumers still open the path and report a clear error on miss.
        if (!base_source.empty()) {
            const std::size_t slash = base_source.find_last_of('/');
            std::string composed = (slash == std::string_view::npos ? std::string(base_source) //
                                                                    : std::string(base_source.substr(0, slash)));
            if (!composed.empty() && composed.back() != '/') {
                composed.push_back('/');
            }
            composed.append(path);
            return composed;
        }
        return std::string(path);
    }
}

std::string canonicalize_path(std::string_view path) {
    namespace fs = std::filesystem;
    try {
        // weakly_canonical resolves symlinks for the existing leading prefix and
        // lexically normalizes the remainder; it does not require `path` to exist.
        return fs::weakly_canonical(fs::path(path)).string();
    } catch (const std::filesystem::filesystem_error &) {
        try {
            return fs::absolute(path).lexically_normal().string();
        } catch (const std::exception &) {
            return std::string(path);
        }
    } catch (const std::exception &) {
        return std::string(path);
    }
}

} // namespace fiber::lite_nginx::config
