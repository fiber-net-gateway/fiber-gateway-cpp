#include "config/LlmConfigStatus.h"

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace fiber::ai_server {

TEST(LlmConfigStatusTest, RendersOnlyActiveResourceEvidenceAndWorkerConvergence) {
    auto bt1 = std::make_shared<Bt1KeySnapshot>();
    bt1->metadata = {.data_id = std::string(kBt1KeysDataId),
                     .group = std::string(kLlmConfigGroup),
                     .md5 = "bt1-md5",
                     .version = 2};
    auto provider_config = std::make_shared<ProviderConfigSnapshot>();
    provider_config->metadata = {.data_id = std::string(kProviderDataIdPrefix) + "openai",
                                 .group = std::string(kLlmConfigGroup),
                                 .md5 = "provider-md5",
                                 .version = 3};
    auto provider = std::make_shared<ProjectProvider>();
    provider->name = "openai";
    provider->config = provider_config;
    auto group = std::make_shared<UserGroupSnapshot>();
    group->metadata = {.data_id = std::string(kUserGroupDataIdPrefix) + "staff",
                       .group = std::string(kLlmConfigGroup),
                       .md5 = "group-md5",
                       .version = 4};
    CompiledModelRoute model;
    model.model_name = "chat";
    model.providers.push_back(provider);
    model.allow_user_groups.push_back(group);
    auto project =
            std::make_shared<LlmProjectSnapshot>(ConfigMetadata{.data_id = std::string(kModelsDataId),
                                                                .group = std::string(kLlmConfigGroup),
                                                                .md5 = "models-md5",
                                                                .version = 5},
                                                 9, std::vector<std::shared_ptr<const ProjectProvider>>{provider},
                                                 std::vector<CompiledModelRoute>{std::move(model)});

    const LlmConfigSnapshot snapshot{.generation = 11, .bt1_keys = bt1, .project = project};
    const std::vector<std::uint64_t> generations{11, 11};
    const std::string status = render_llm_config_status(snapshot, 1, generations);

    EXPECT_NE(status.find("\"state\":\"ACTIVE\""), std::string::npos);
    EXPECT_NE(status.find("\"workerIndex\":1"), std::string::npos);
    EXPECT_NE(status.find("\"generations\":[11,11]"), std::string::npos);
    EXPECT_NE(status.find("ploto.ai-llm.auth.bt1.keys"), std::string::npos);
    EXPECT_NE(status.find("ploto.ai-llm.models"), std::string::npos);
    EXPECT_NE(status.find("ploto.ai-llm.provider.openai"), std::string::npos);
    EXPECT_NE(status.find("ploto.ai-llm.user-group.staff"), std::string::npos);
    EXPECT_EQ(status.find("secret"), std::string::npos);
}

TEST(LlmConfigStatusTest, ReportsCatchingUpWhenAnyWorkerUsesAnotherGeneration) {
    const LlmConfigSnapshot snapshot{.generation = 7};
    const std::vector<std::uint64_t> generations{7, 6};
    const std::string status = render_llm_config_status(snapshot, 0, generations);

    EXPECT_NE(status.find("\"state\":\"CATCHING_UP\""), std::string::npos);
    EXPECT_NE(status.find("\"converged\":false"), std::string::npos);
}

} // namespace fiber::ai_server
