#include "LlmConfigStatus.h"

#include <algorithm>
#include <charconv>
#include <string_view>
#include <vector>

namespace fiber::ai_server {

namespace {

void append_uint(std::string &output, std::uint64_t value) {
    char buffer[32];
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
    output.append(buffer, result.ptr);
}

void append_int(std::string &output, std::int32_t value) {
    char buffer[16];
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
    output.append(buffer, result.ptr);
}

void append_json_string(std::string &output, std::string_view value) {
    constexpr char kHex[] = "0123456789abcdef";
    output.push_back('"');
    for (const unsigned char character: value) {
        switch (character) {
            case '"':
                output.append("\\\"");
                break;
            case '\\':
                output.append("\\\\");
                break;
            case '\b':
                output.append("\\b");
                break;
            case '\f':
                output.append("\\f");
                break;
            case '\n':
                output.append("\\n");
                break;
            case '\r':
                output.append("\\r");
                break;
            case '\t':
                output.append("\\t");
                break;
            default:
                if (character < 0x20) {
                    output.append("\\u00");
                    output.push_back(kHex[character >> 4]);
                    output.push_back(kHex[character & 0x0f]);
                } else {
                    output.push_back(static_cast<char>(character));
                }
                break;
        }
    }
    output.push_back('"');
}

void add_metadata(std::vector<const ConfigMetadata *> &resources, const ConfigMetadata &metadata) {
    const auto existing = std::ranges::find(resources, metadata.data_id, [](const ConfigMetadata *candidate) {
        return std::string_view(candidate->data_id);
    });
    if (existing == resources.end()) {
        resources.push_back(&metadata);
    }
}

} // namespace

std::string render_llm_config_status(const LlmConfigSnapshot &snapshot, std::size_t worker_index,
                                     std::span<const std::uint64_t> worker_generations) {
    std::vector<const ConfigMetadata *> resources;
    if (snapshot.bt1_keys) {
        add_metadata(resources, snapshot.bt1_keys->metadata);
    }
    if (snapshot.project) {
        add_metadata(resources, snapshot.project->metadata());
        for (const auto &provider: snapshot.project->providers()) {
            if (provider && provider->config) {
                add_metadata(resources, provider->config->metadata);
            }
        }
        for (const auto &model: snapshot.project->models()) {
            for (const auto &group: model.allow_user_groups) {
                if (group) {
                    add_metadata(resources, group->metadata);
                }
            }
        }
    }
    std::ranges::sort(resources, {},
                      [](const ConfigMetadata *metadata) { return std::string_view(metadata->data_id); });

    const bool converged = !worker_generations.empty() &&
                           std::ranges::all_of(worker_generations, [generation = snapshot.generation](auto current) {
                               return current == generation;
                           });
    std::string output;
    output.reserve(256 + resources.size() * 128 + worker_generations.size() * 24);
    output.append("{\"schemaVersion\":1,\"state\":\"");
    output.append(converged ? "ACTIVE" : "CATCHING_UP");
    output.append("\",\"generation\":");
    append_uint(output, snapshot.generation);
    output.append(",\"workerIndex\":");
    append_uint(output, worker_index);
    output.append(",\"workers\":{\"count\":");
    append_uint(output, worker_generations.size());
    output.append(",\"converged\":");
    output.append(converged ? "true" : "false");
    output.append(",\"generations\":[");
    for (std::size_t i = 0; i < worker_generations.size(); ++i) {
        if (i != 0) {
            output.push_back(',');
        }
        append_uint(output, worker_generations[i]);
    }
    output.append("]},\"resources\":[");
    for (std::size_t i = 0; i < resources.size(); ++i) {
        if (i != 0) {
            output.push_back(',');
        }
        const ConfigMetadata &metadata = *resources[i];
        output.append("{\"dataId\":");
        append_json_string(output, metadata.data_id);
        output.append(",\"group\":");
        append_json_string(output, metadata.group);
        output.append(",\"md5\":");
        append_json_string(output, metadata.md5);
        output.append(",\"version\":");
        append_int(output, metadata.version);
        output.push_back('}');
    }
    output.append("]}\n");
    return output;
}

} // namespace fiber::ai_server
