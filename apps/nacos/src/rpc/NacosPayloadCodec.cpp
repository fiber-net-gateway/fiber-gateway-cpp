#include "NacosPayloadCodec.h"

#include <limits>
#include <string>

namespace fiber::nacos::detail {
namespace {

NacosRpcError protocol_error(common::IoErr io_error, std::string message) {
    return NacosRpcError{
            .code = NacosRpcErrorCode::Protocol,
            .io_error = io_error,
            .message = std::move(message),
    };
}

void add_header(proto::Metadata &metadata, std::string_view key, std::string_view value) {
    (*metadata.mutable_headers())[std::string(key)] = std::string(value);
}

} // namespace

std::expected<NacosPayloadView, NacosRpcError> validate_payload(const proto::Payload &payload,
                                                                std::size_t max_payload_bytes) noexcept {
    if (payload.ByteSizeLong() > max_payload_bytes || payload.body().value().size() > max_payload_bytes) {
        return std::unexpected(protocol_error(common::IoErr::MessageTooLarge, "Nacos Payload exceeds limit"));
    }
    if (!payload.has_metadata() || !payload.has_body() || payload.metadata().type().empty()) {
        return std::unexpected(protocol_error(common::IoErr::Invalid, "incomplete Nacos Payload"));
    }
    if (!payload.body().type_url().empty()) {
        return std::unexpected(protocol_error(common::IoErr::Invalid, "unexpected Nacos Any type_url"));
    }
    return NacosPayloadView{
            .type = payload.metadata().type(),
            .json = payload.body().value(),
    };
}

std::expected<proto::Payload, NacosRpcError> encode_payload_json(std::string_view type, std::string_view json,
                                                                 const NacosPayloadMetadata &metadata,
                                                                 std::size_t max_payload_bytes) noexcept {
    if (type.empty() || json.size() > max_payload_bytes ||
        json.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(protocol_error(common::IoErr::MessageTooLarge, "Nacos JSON exceeds payload limit"));
    }

    proto::Payload payload;
    proto::Metadata *wire_metadata = payload.mutable_metadata();
    wire_metadata->set_type(type.data(), type.size());
    wire_metadata->set_clientip(metadata.client_ip.data(), metadata.client_ip.size());
    add_header(*wire_metadata, "clientIp", metadata.client_ip);
    add_header(*wire_metadata, "clientVersion", metadata.client_version);
    add_header(*wire_metadata, "namespace", metadata.namespace_id);
    if (metadata.access_token) {
        add_header(*wire_metadata, "accessToken", *metadata.access_token);
    }
    payload.mutable_body()->set_value(json.data(), json.size());

    if (payload.ByteSizeLong() > max_payload_bytes) {
        return std::unexpected(protocol_error(common::IoErr::MessageTooLarge, "Nacos Payload exceeds limit"));
    }
    return payload;
}

} // namespace fiber::nacos::detail
