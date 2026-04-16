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
};

[[nodiscard]] common::IoErr http_scan_origin_form_uri(std::string_view raw_uri, HttpUriParseState &state) noexcept;

[[nodiscard]] common::IoErr http_finalize_request_uri(std::string_view raw_uri, const HttpUriParseState &state,
                                                      HttpUri &uri, mem::BufPool *pool = nullptr,
                                                      bool merge_slashes = true) noexcept;

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_URI_PARSE_H
