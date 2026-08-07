#include "CatRouter.h"

#include <charconv>
#include <cmath>
#include <string_view>

#include <fiber/common/json/JsonParse.h>
#include <fiber/common/json/JsonParser.h>
#include <fiber/common/mem/BufPool.h>
#include <fiber/net/IpAddress.h>

namespace fiber::cat::detail {

namespace {

struct RouterWire {
    std::string_view routers;
    double sample = 1.0;
    bool block = false;
    bool has_routers = false;
};

bool parse_text_double(std::string_view text, double &out) noexcept {
    if (text.empty()) {
        return false;
    }
    const char *end = text.data() + text.size();
    auto result = std::from_chars(text.data(), end, out);
    return result.ec == std::errc{} && result.ptr == end && std::isfinite(out);
}

json::ParseStatus parse_flexible_double(json::JsonParser &parser, mem::BufPool &pool, double &out) noexcept {
    const json::Token *token = parser.current_token();
    if (token && token->kind == json::TokenKind::Text) {
        std::string_view text;
        if (json::parse_text(parser, pool, text) != json::ParseStatus::Done || !parse_text_double(text, out)) {
            (void) parser.fail("invalid numeric string");
            return json::ParseStatus::Error;
        }
        return json::ParseStatus::Done;
    }
    return json::parse_double(parser, pool, out);
}

json::ParseStatus parse_flexible_bool(json::JsonParser &parser, mem::BufPool &pool, bool &out) noexcept {
    const json::Token *token = parser.current_token();
    if (token && token->kind == json::TokenKind::Text) {
        std::string_view text;
        if (json::parse_text(parser, pool, text) != json::ParseStatus::Done) {
            return json::ParseStatus::Error;
        }
        if (text == "true" || text == "TRUE" || text == "yes" || text == "YES" || text == "1") {
            out = true;
            return json::ParseStatus::Done;
        }
        if (text == "false" || text == "FALSE" || text == "no" || text == "NO" || text == "0") {
            out = false;
            return json::ParseStatus::Done;
        }
        (void) parser.fail("invalid boolean string");
        return json::ParseStatus::Error;
    }
    return json::parse_bool(parser, pool, out);
}

json::ObjectFieldStatus parse_kvs_field(std::string_view field, json::JsonParser &parser, mem::BufPool &pool,
                                        RouterWire &wire) noexcept {
    if (field == "routers") {
        if (json::parse_text(parser, pool, wire.routers) != json::ParseStatus::Done) {
            return json::ObjectFieldStatus::Error;
        }
        wire.has_routers = true;
        return json::ObjectFieldStatus::Parsed;
    }
    if (field == "sample") {
        return parse_flexible_double(parser, pool, wire.sample) == json::ParseStatus::Done
                       ? json::ObjectFieldStatus::Parsed
                       : json::ObjectFieldStatus::Error;
    }
    if (field == "block") {
        return parse_flexible_bool(parser, pool, wire.block) == json::ParseStatus::Done
                       ? json::ObjectFieldStatus::Parsed
                       : json::ObjectFieldStatus::Error;
    }
    return json::ObjectFieldStatus::Unknown;
}

json::ObjectFieldStatus parse_root_field(std::string_view field, json::JsonParser &parser, mem::BufPool &pool,
                                         RouterWire &wire) noexcept {
    if (field != "kvs") {
        return json::ObjectFieldStatus::Unknown;
    }
    return json::parse_object_fields(parser, pool, wire, parse_kvs_field) == json::ParseStatus::Done
                   ? json::ObjectFieldStatus::Parsed
                   : json::ObjectFieldStatus::Error;
}

bool parse_port(std::string_view text, std::uint16_t &port) noexcept {
    unsigned value = 0;
    const char *end = text.data() + text.size();
    auto result = std::from_chars(text.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end || value == 0 || value > 65535) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

bool parse_collector(std::string_view text, net::SocketAddress &out) noexcept {
    std::string_view address;
    std::string_view port_text;
    if (text.size() >= 4 && text.front() == '[') {
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos || close + 2 > text.size() || text[close + 1] != ':') {
            return false;
        }
        address = text.substr(1, close - 1);
        port_text = text.substr(close + 2);
    } else {
        const std::size_t colon = text.rfind(':');
        if (colon == std::string_view::npos) {
            return false;
        }
        address = text.substr(0, colon);
        port_text = text.substr(colon + 1);
    }

    net::IpAddress ip;
    std::uint16_t port = 0;
    if (!net::IpAddress::parse(address, ip) || !parse_port(port_text, port) || ip.is_unspecified()) {
        return false;
    }
    out = net::SocketAddress(ip, port);
    return true;
}

} // namespace

std::expected<RouterSnapshot, RouterParseError> parse_router_response(std::string_view input,
                                                                      std::size_t max_collectors) noexcept {
    if (max_collectors == 0) {
        return std::unexpected(RouterParseError::TooManyCollectors);
    }

    json::JsonParser parser;
    mem::BufPool pool;
    if (!parser.feed(input.data(), input.size())) {
        return std::unexpected(RouterParseError::InvalidJson);
    }
    parser.finish();

    RouterWire wire;
    if (json::parse_document(parser, pool, wire,
                             [](json::JsonParser &document, mem::BufPool &document_pool, RouterWire &value) noexcept {
                                 return json::parse_object_fields(document, document_pool, value, parse_root_field);
                             }) != json::ParseStatus::Done) {
        return std::unexpected(RouterParseError::InvalidJson);
    }
    if (!wire.has_routers || wire.sample < 0.0 || wire.sample > 1.0 || !std::isfinite(wire.sample)) {
        return std::unexpected(RouterParseError::InvalidResponse);
    }

    RouterSnapshot snapshot;
    snapshot.sample = wire.sample;
    snapshot.block = wire.block;
    std::size_t offset = 0;
    while (offset <= wire.routers.size()) {
        const std::size_t separator = wire.routers.find(';', offset);
        const std::size_t end = separator == std::string_view::npos ? wire.routers.size() : separator;
        const std::string_view item = wire.routers.substr(offset, end - offset);
        if (!item.empty()) {
            if (snapshot.collectors.size() >= max_collectors) {
                return std::unexpected(RouterParseError::TooManyCollectors);
            }
            net::SocketAddress collector;
            if (!parse_collector(item, collector)) {
                return std::unexpected(RouterParseError::InvalidCollector);
            }
            bool duplicate = false;
            for (const net::SocketAddress &existing: snapshot.collectors) {
                if (existing.ip() == collector.ip() && existing.port() == collector.port()) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                snapshot.collectors.push_back(collector);
            }
        }
        if (separator == std::string_view::npos) {
            break;
        }
        offset = separator + 1;
    }
    if (!wire.block && snapshot.collectors.empty()) {
        return std::unexpected(RouterParseError::InvalidResponse);
    }
    return snapshot;
}

} // namespace fiber::cat::detail
