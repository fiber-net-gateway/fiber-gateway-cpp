#include <fiber/http/Http3Client.h>

#include <utility>

namespace fiber::http {

Http3Client::Http3Client(quic::QuicUdpEndpoint &endpoint, Options options) noexcept :
    endpoint_(endpoint), options_(std::move(options)), alpn_({"h3"}) {}

} // namespace fiber::http
