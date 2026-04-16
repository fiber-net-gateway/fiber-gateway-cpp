#ifndef FIBER_HTTP_HTTP_URI_PARSE_H
#define FIBER_HTTP_HTTP_URI_PARSE_H

#include <cstddef>
#include <string_view>

#include "../common/IoError.h"
#include "../common/mem/BufPool.h"
#include "HttpCommon.h"

namespace fiber::http {

struct HttpUriParseState {
    bool complex_uri = false;
    bool quoted_uri = false;
    bool plus_in_uri = false;
    bool empty_path_in_uri = false;
    std::size_t query_pos = 0;
    std::size_t fragment_pos = 0;
    std::size_t exten_pos = 0;
    bool has_query = false;
    bool has_fragment = false;
    bool has_exten = false;

    [[nodiscard]] bool needs_complex_parse() const noexcept {
        return complex_uri || quoted_uri || empty_path_in_uri;
    }
};

[[nodiscard]] common::IoErr http_parse_uri(std::string_view raw_uri, HttpUriParseState &state) noexcept;

[[nodiscard]] common::IoErr http_process_uri(std::string_view raw_uri, const HttpUriParseState &state, HttpUri &uri,
                                             mem::BufPool *pool = nullptr, bool merge_slashes = true) noexcept;

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_URI_PARSE_H
