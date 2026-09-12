#include <fiber/http/Http3Client.h>

#include <utility>

namespace fiber::http {

Http3Client::Http3Client(quic::QuicUdpEndpoint &endpoint, Options options) noexcept :
    endpoint_(endpoint), options_(std::move(options)), alpn_({"h3"}) {
    if (options_.local_settings.max_field_section_size == 0) {
        options_.local_settings.max_field_section_size = options_.max_field_section_size;
    }
}

} // namespace fiber::http
