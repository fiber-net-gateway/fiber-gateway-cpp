#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include <fiber/nacos/dto/JsonCodec.h>

namespace {

using fiber::json::Generator;
using fiber::json::JsonObject;
using fiber::json::JsonParser;
using fiber::json::OutputSink;
using fiber::json::ParseStatus;
using fiber::mem::BufPool;
namespace dto = fiber::nacos::dto;

class StringSink final : public OutputSink {
public:
    [[nodiscard]] bool write(const char *data, std::size_t len) override {
        output.append(data ? data : "", len);
        return data != nullptr || len == 0;
    }

    std::string output;
};

template<typename T>
std::string encode(const T &value) {
    StringSink sink;
    Generator generator(sink);
    EXPECT_EQ(dto::encode_json(generator, value), Generator::Result::OK);
    return sink.output;
}

template<typename T>
ParseStatus parse(std::string_view input, BufPool &pool, T &out, JsonParser &parser) {
    if (!parser.feed(input.data(), input.size())) {
        return ParseStatus::Error;
    }
    parser.finish();
    auto value_parser = [](JsonParser &value_parser, BufPool &value_pool, T &value) noexcept {
        return dto::parse_json(value_parser, value_pool, value);
    };
    return fiber::json::parse_document(parser, pool, out, value_parser);
}

TEST(NacosDtoJsonTest, InternalRequestsMatchJavaWireJson) {
    EXPECT_EQ(encode(dto::req::ServerCheckRequest{}), R"({"requestId":null,"module":"internal"})");
    EXPECT_EQ(encode(dto::req::HealthCheckRequest{}), R"({"requestId":null,"module":"internal"})");
    EXPECT_EQ(encode(dto::req::ClientDetectionRequest{}), R"({"requestId":null,"module":"internal"})");

    JsonObject<std::string_view>::Entry labels[] = {
            {.key = "source", .value = "sdk"},
            {.key = "module", .value = "config"},
    };
    dto::req::ConnectionSetupRequest setup;
    setup.client_version.set_present("client");
    setup.tenant.set_present("tenant");
    setup.labels.set_present(JsonObject<std::string_view>(labels, std::size(labels)));
    setup.ability_table.set_present(JsonObject<bool>());
    EXPECT_EQ(
            encode(setup),
            R"({"requestId":null,"clientVersion":"client","tenant":"tenant","labels":{"source":"sdk","module":"config"},"abilityTable":{},"module":"internal"})");

    EXPECT_EQ(encode(dto::req::SetupAckRequest{}), R"({"requestId":null,"abilityTable":null,"module":"internal"})");
    EXPECT_EQ(encode(dto::req::ConnectResetRequest{}),
              R"({"requestId":null,"serverIp":null,"serverPort":null,"connectionId":null,"module":"internal"})");
}

TEST(NacosDtoJsonTest, InternalResponsesMatchJavaWireJson) {
    dto::resp::ServerCheckResponse check;
    EXPECT_EQ(
            encode(check),
            R"({"resultCode":200,"errorCode":0,"message":null,"requestId":null,"connectionId":null,"supportAbilityNegotiation":false,"success":true})");
    EXPECT_EQ(encode(dto::resp::HealthCheckResponse{}),
              R"({"resultCode":200,"errorCode":0,"message":null,"requestId":null,"success":true})");
    EXPECT_EQ(encode(dto::resp::ClientDetectionResponse{}),
              R"({"resultCode":200,"errorCode":0,"message":null,"requestId":null,"success":true})");
    EXPECT_EQ(encode(dto::resp::ConnectResetResponse{}),
              R"({"resultCode":200,"errorCode":0,"message":null,"requestId":null,"success":true})");
    EXPECT_EQ(encode(dto::resp::ErrorResponse{}),
              R"({"resultCode":200,"errorCode":0,"message":null,"requestId":null,"success":true})");
}

TEST(NacosDtoJsonTest, ConfigRequestsMatchJavaWireJson) {
    dto::req::ConfigPublishRequest publish;
    publish.data_id.set_present("d");
    publish.group.set_present("g");
    publish.tenant.set_present("t");
    publish.content.set_present("c");
    publish.cas_md5.set_present("md5");
    JsonObject<std::string_view>::Entry additions[] = {{.key = "type", .value = "json"}};
    publish.addition_map.set_present(JsonObject<std::string_view>(additions, std::size(additions)));
    EXPECT_EQ(
            encode(publish),
            R"({"requestId":null,"dataId":"d","group":"g","tenant":"t","content":"c","casMd5":"md5","additionMap":{"type":"json"},"module":"config"})");

    dto::req::ConfigRemoveRequest remove;
    remove.data_id.set_present("d");
    remove.group.set_present("g");
    remove.tenant.set_present("t");
    EXPECT_EQ(encode(remove),
              R"({"requestId":null,"dataId":"d","group":"g","tenant":"t","tag":null,"module":"config"})");

    dto::req::ConfigListenContext context;
    context.group.set_present("g");
    context.md5.set_null();
    context.data_id.set_present("d");
    context.tenant.set_present("t");
    dto::req::ConfigBatchListenRequest listen;
    listen.config_listen_contexts = fiber::json::JsonArray<dto::req::ConfigListenContext>(&context, 1);
    EXPECT_EQ(
            encode(listen),
            R"({"requestId":null,"dataId":null,"group":null,"tenant":null,"listen":true,"configListenContexts":[{"group":"g","md5":null,"dataId":"d","tenant":"t"}],"module":"config"})");
}

TEST(NacosDtoJsonTest, ConfigResponsesMatchJavaWireJsonAndRoundTrip) {
    dto::resp::ConfigQueryResponse query;
    query.content.set_present("");
    query.encrypted_data_key.set_present("e");
    query.content_type.set_present("yaml");
    query.md5.set_present("m");
    query.last_modified = 7;
    query.tag.set_present("tag");
    query.beta = true;
    constexpr std::string_view Json =
            R"({"resultCode":200,"errorCode":0,"message":null,"requestId":null,"content":"","encryptedDataKey":"e","contentType":"yaml","md5":"m","lastModified":7,"tag":"tag","beta":true,"success":true})";
    EXPECT_EQ(encode(query), Json);

    BufPool pool;
    JsonParser parser;
    dto::resp::ConfigQueryResponse decoded;
    ASSERT_EQ(parse(Json, pool, decoded, parser), ParseStatus::Done);
    ASSERT_TRUE(decoded.content.is_present());
    EXPECT_TRUE(decoded.content.value().empty());
    ASSERT_TRUE(decoded.md5.is_present());
    EXPECT_EQ(decoded.md5.value(), "m");
    EXPECT_EQ(decoded.last_modified, 7);
    EXPECT_TRUE(decoded.beta);
    EXPECT_EQ(encode(decoded), Json);

    dto::resp::ConfigContext changed;
    changed.group.set_present("g");
    changed.data_id.set_present("d");
    changed.tenant.set_present("t");
    dto::resp::ConfigChangeBatchListenResponse batch;
    batch.changed_configs = fiber::json::JsonArray<dto::resp::ConfigContext>(&changed, 1);
    EXPECT_EQ(
            encode(batch),
            R"({"resultCode":200,"errorCode":0,"message":null,"requestId":null,"changedConfigs":[{"group":"g","dataId":"d","tenant":"t"}],"success":true})");
}

TEST(NacosDtoJsonTest, RpcDtoParsingIsTransactionalAndRejectsWrongTypes) {
    dto::resp::ServerCheckResponse response;
    response.connection_id.set_present("old");
    response.support_ability_negotiation = true;
    BufPool pool;
    JsonParser parser;
    EXPECT_EQ(parse(R"({"supportAbilityNegotiation":"yes"})", pool, response, parser), ParseStatus::Error);
    ASSERT_TRUE(response.connection_id.is_present());
    EXPECT_EQ(response.connection_id.value(), "old");
    EXPECT_TRUE(response.support_ability_negotiation);
}

} // namespace
