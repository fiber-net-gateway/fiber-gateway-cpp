#include <gtest/gtest.h>

#include "mcp/McpJsonCodec.h"

TEST(McpJsonCodecTest, ParsesAndSortsVersionedNameSet) {
    auto parsed =
            fiber::ai_server::parse_mcp_name_set_config(R"({"version":7,"data":["weather","admin_tools"]})", true);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->version, 7);
    ASSERT_EQ(parsed->names.size(), 2u);
    EXPECT_EQ(parsed->names[0], "admin_tools");
    EXPECT_EQ(parsed->names[1], "weather");
}

TEST(McpJsonCodecTest, RejectsDuplicateAndUnsafeNames) {
    auto duplicate = fiber::ai_server::parse_mcp_name_set_config(R"({"version":1,"data":["weather","weather"]})", true);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, fiber::ai_server::McpJsonErrorCode::DuplicateValue);

    auto unsafe = fiber::ai_server::parse_mcp_name_set_config(R"({"version":1,"data":["../weather"]})", true);
    ASSERT_FALSE(unsafe);
    EXPECT_EQ(unsafe.error().code, fiber::ai_server::McpJsonErrorCode::InvalidField);
}

TEST(McpJsonCodecTest, ParsesAdminToolAndJavaCompatibleCache) {
    constexpr std::string_view kPayload = R"({
        "id":"weather.current",
        "script":"return {city: city, ok: true};",
        "tool":{
            "name":"weather_current",
            "description":"Current weather",
            "inputSchema":{
                "type":"object",
                "properties":{"city":{"type":"string"}},
                "required":["city"],
                "additionalProperties":false
            }
        }
    })";
    auto loaded = fiber::ai_server::parse_mcp_admin_tool(kPayload, "weather.current");
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->descriptor.name, "weather_current");
    EXPECT_EQ(loaded->descriptor.description, "Current weather");
    EXPECT_EQ(loaded->script, "return {city: city, ok: true};");
    EXPECT_NE(loaded->descriptor.input_schema_json.find("\"required\":[\"city\"]"), std::string::npos);

    const std::string cache = fiber::ai_server::encode_mcp_tool_cache(*loaded);
    EXPECT_TRUE(cache.starts_with("```\nreturn {city: city, ok: true};\n```\n"));
    auto restored = fiber::ai_server::parse_mcp_tool_cache(cache, "weather.current");
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->descriptor.script_id, loaded->descriptor.script_id);
    EXPECT_EQ(restored->descriptor.tool_json, loaded->descriptor.tool_json);
    EXPECT_EQ(restored->script, loaded->script);
}

TEST(McpConfigSnapshotTest, LooksUpSortedProjectAndTool) {
    auto tool = std::make_shared<fiber::ai_server::McpTool>();
    tool->descriptor.name = "weather_current";
    auto project = std::make_shared<fiber::ai_server::McpProjectRuntime>();
    project->name = "weather";
    project->tools.push_back(tool);
    fiber::ai_server::McpConfigSnapshot snapshot;
    snapshot.projects.push_back(project);

    ASSERT_EQ(snapshot.find_project("weather"), project.get());
    ASSERT_EQ(project->find_tool("weather_current"), tool.get());
    EXPECT_EQ(snapshot.find_project("missing"), nullptr);
    EXPECT_EQ(project->find_tool("missing"), nullptr);
}
