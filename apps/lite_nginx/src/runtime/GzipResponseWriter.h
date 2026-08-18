#ifndef FIBER_LITE_NGINX_RUNTIME_GZIP_RESPONSE_WRITER_H
#define FIBER_LITE_NGINX_RUNTIME_GZIP_RESPONSE_WRITER_H

#include <fiber/http/GzipResponseWriter.h>

namespace fiber::lite_nginx::runtime {

using GzipResponseWriterOptions = fiber::http::GzipResponseWriterOptions;
using GzipResponseDecision = fiber::http::GzipResponseDecision;
using GzipResponseWriterStats = fiber::http::GzipResponseWriterStats;
using GzipResponseWriter = fiber::http::GzipResponseWriter;

} // namespace fiber::lite_nginx::runtime

#endif // FIBER_LITE_NGINX_RUNTIME_GZIP_RESPONSE_WRITER_H
