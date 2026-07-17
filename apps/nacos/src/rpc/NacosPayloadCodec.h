#ifndef FIBER_NACOS_RPC_NACOS_PAYLOAD_CODEC_H
#define FIBER_NACOS_RPC_NACOS_PAYLOAD_CODEC_H

#include <cstddef>
#include <expected>
#include <string_view>

#include <common/json/JsonParser.h>
#include <common/mem/BufPool.h>
#include <fiber/nacos/dto/JsonCodec.h>
#include <nacos_grpc_payload.pb.h>

#include "NacosRpcError.h"

namespace fiber::nacos::detail {

struct NacosPayloadMetadata {
    std::string_view client_ip;
    std::string_view client_version;
    std::string_view namespace_id;
    std::string_view access_token;
};

struct NacosPayloadView {
    std::string_view type;
    std::string_view json;
};

[[nodiscard]] std::expected<NacosPayloadView, NacosRpcError> validate_payload(const proto::Payload &payload,
                                                                              std::size_t max_payload_bytes) noexcept;

[[nodiscard]] std::expected<proto::Payload, NacosRpcError> encode_payload_json(std::string_view type,
                                                                               std::string_view json,
                                                                               const NacosPayloadMetadata &metadata,
                                                                               std::size_t max_payload_bytes) noexcept;

template<typename T>
[[nodiscard]] std::expected<proto::Payload, NacosRpcError>
encode_payload(const T &value, const NacosPayloadMetadata &metadata, std::size_t max_payload_bytes) noexcept {
    class BoundedStringSink final : public json::OutputSink {
    public:
        BoundedStringSink(std::string &output, std::size_t limit) noexcept : output_(&output), limit_(limit) {}

        [[nodiscard]] bool write(const char *data, std::size_t len) override {
            if ((!data && len > 0) || len > limit_ - output_->size()) {
                too_large_ = true;
                return false;
            }
            output_->append(data ? data : "", len);
            return true;
        }

        void reset() override { output_->clear(); }

        [[nodiscard]] bool too_large() const noexcept { return too_large_; }

    private:
        std::string *output_ = nullptr;
        std::size_t limit_ = 0;
        bool too_large_ = false;
    };

    std::string json_body;
    BoundedStringSink sink(json_body, max_payload_bytes);
    json::Generator generator(sink);
    if (dto::encode_json(generator, value) != json::Generator::Result::OK) {
        return std::unexpected(NacosRpcError{
                .code = NacosRpcErrorCode::Protocol,
                .io_error = sink.too_large() ? common::IoErr::MessageTooLarge : common::IoErr::Invalid,
                .message = sink.too_large() ? "Nacos JSON exceeds payload limit" : "failed to encode Nacos JSON",
        });
    }
    return encode_payload_json(T::kTypeName, json_body, metadata, max_payload_bytes);
}

template<typename T>
[[nodiscard]] std::expected<void, NacosRpcError> parse_payload_json(const NacosPayloadView &payload, mem::BufPool &pool,
                                                                    T &out) noexcept {
    json::JsonParser parser;
    if (!parser.feed(payload.json.data(), payload.json.size())) {
        return std::unexpected(NacosRpcError{
                .code = NacosRpcErrorCode::Protocol,
                .io_error = common::IoErr::Invalid,
                .message = "invalid Nacos JSON",
        });
    }
    parser.finish();
    auto value_parser = [](json::JsonParser &value_parser, mem::BufPool &value_pool, T &value) noexcept {
        return dto::parse_json(value_parser, value_pool, value);
    };
    if (json::parse_document(parser, pool, out, value_parser) != json::ParseStatus::Done) {
        std::string message = "invalid Nacos JSON";
        if (parser.error().message) {
            message.append(": ");
            message.append(parser.error().message);
        }
        if (message.size() > 512) {
            message.resize(512);
        }
        return std::unexpected(NacosRpcError{
                .code = NacosRpcErrorCode::Protocol,
                .io_error = common::IoErr::Invalid,
                .message = std::move(message),
        });
    }
    return {};
}

template<typename T>
[[nodiscard]] std::expected<void, NacosRpcError>
decode_payload(const proto::Payload &payload, std::size_t max_payload_bytes, mem::BufPool &pool, T &out) noexcept {
    auto view = validate_payload(payload, max_payload_bytes);
    if (!view) {
        return std::unexpected(std::move(view.error()));
    }
    if (view->type == dto::resp::ErrorResponse::kTypeName) {
        dto::resp::ErrorResponse response;
        auto parsed = parse_payload_json(*view, pool, response);
        if (!parsed) {
            return parsed;
        }
        NacosRpcError error{
                .code = NacosRpcErrorCode::Server,
                .result_code = response.result_code,
                .error_code = response.error_code,
        };
        if (response.message.is_present()) {
            error.message.assign(response.message.value().substr(0, 512));
        }
        return std::unexpected(std::move(error));
    }
    if (view->type != T::kTypeName) {
        return std::unexpected(NacosRpcError{
                .code = NacosRpcErrorCode::Protocol,
                .io_error = common::IoErr::Invalid,
                .message = "Nacos response type mismatch",
        });
    }
    return parse_payload_json(*view, pool, out);
}

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_RPC_NACOS_PAYLOAD_CODEC_H
