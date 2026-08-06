#include "McpScriptRuntime.h"

#include "McpScriptServices.h"

#include <deque>
#include <utility>
#include <vector>

#include <http_script/HttpClientFuncs.h>
#include <http_script/HttpScriptLib.h>
#include <http_script/HttpScriptServices.h>
#include <http_script/HttpTarget.h>
#include <http_script/ScriptExchangeCtx.h>
#include <script/JsGc.h>
#include <script/JsValue.h>
#include <script/Library.h>
#include <script/Script.h>
#include <script/ScriptCompiler.h>
#include <script/gc/GcInternal.h>
#include <script/json/JsValueDecode.h>
#include <script/json/JsValueEncode.h>
#include <script/std/NodeText.h>
#include <script/std/StdLibrary.h>

namespace fiber::ai_server {
namespace {

constexpr char kServicePrefix[] = "mcp-service:";
constexpr char kAddressPrefix[] = "mcp-address:";

class McpDirectiveExtension final {
public:
    using Library = script::Library;

    explicit McpDirectiveExtension(McpScriptServices *services) noexcept : services_(services) {}

    [[nodiscard]] static const script::std_lib::StdLibrary::ExtOps &ops() noexcept {
        static const script::std_lib::StdLibrary::ExtOps kOps{
                .resolve_directive_def = &resolve_directive_def_op,
        };
        return kOps;
    }

private:
    static Library::DirectiveDef *resolve_directive_def_op(void *context, std::string_view type, std::string_view,
                                                           const std::vector<script::JsValue> &literals) {
        return static_cast<McpDirectiveExtension *>(context)->resolve(type, literals);
    }

    Library::DirectiveDef *resolve(std::string_view type, const std::vector<script::JsValue> &literals) {
        if (type == "service" && literals.size() == 1) {
            std::string_view literal;
            if (!script::std_lib::string_utf8_view(literals[0], literal) || literal.empty()) {
                return nullptr;
            }
            std::string service(literal);
            std::string cluster = "default";
            const std::size_t slash = service.find('/');
            if (slash != std::string::npos) {
                cluster.assign(service, slash + 1, std::string::npos);
                service.resize(slash);
            }
            if (service.empty() || cluster.empty()) {
                return nullptr;
            }
            service.append(".app");
            http_script::HttpTargetSpec target;
            target.kind = http_script::HttpTargetSpec::Kind::Upstream;
            target.name.reserve(sizeof(kServicePrefix) + service.size() + cluster.size());
            target.name.append(kServicePrefix, sizeof(kServicePrefix) - 1);
            target.name.append(service);
            target.name.push_back('/');
            target.name.append(cluster);
            if (services_ && !services_->observe_target(target)) {
                return nullptr;
            }
            definitions_.push_back(std::make_unique<http_script::HttpDirectiveDef>(std::move(target)));
            return definitions_.back().get();
        }
        if (type == "address" && !literals.empty()) {
            http_script::HttpTargetSpec target;
            target.kind = http_script::HttpTargetSpec::Kind::Upstream;
            target.name.append(kAddressPrefix, sizeof(kAddressPrefix) - 1);
            for (const script::JsValue &value: literals) {
                std::string_view literal;
                if (!script::std_lib::string_utf8_view(value, literal) || literal.empty()) {
                    return nullptr;
                }
                if (target.name.size() != sizeof(kAddressPrefix) - 1) {
                    target.name.push_back(',');
                }
                target.name.append(literal);
            }
            if (services_ && !services_->observe_target(target)) {
                return nullptr;
            }
            definitions_.push_back(std::make_unique<http_script::HttpDirectiveDef>(std::move(target)));
            return definitions_.back().get();
        }
        return nullptr;
    }

    std::vector<std::unique_ptr<http_script::HttpDirectiveDef>> definitions_;
    McpScriptServices *services_ = nullptr;
};

struct ScriptEnvironment final {
    explicit ScriptEnvironment(McpScriptServices *services) : directives(services) {
        http_script::register_http_functions_to_lib(library);
        library.add_ext_ops(&directives, McpDirectiveExtension::ops());
    }

    script::std_lib::StdLibrary library;
    McpDirectiveExtension directives;
    script::Script program;
};

std::string exception_message(const script::JsValue &exception) {
    if (script::js_value_type(exception) == script::JsNodeType::Exception && script::js_value_is_heap_ref(exception)) {
        const auto *value = script::js_value_heap_ptr<const script::GcException>(exception);
        std::string message;
        if (value && value->message) {
            script::gc_string_to_utf8(value->message, message);
        }
        return message.empty() ? std::string("script exception") : message;
    }
    if (script::js_value_tag(exception) == script::JsTag::Exception) {
        return std::string(script::exception_kind_name(script::js_value_exception_kind(exception)));
    }
    return "script exception";
}

class ScriptToolHandler final : public McpToolHandler {
public:
    ScriptToolHandler(std::shared_ptr<ScriptEnvironment> environment,
                      http_script::HttpScriptServices *services) noexcept :
        environment_(std::move(environment)), services_(services) {}

    async::Task<McpToolCallResult> invoke(http::HttpExchange &exchange,
                                          std::string_view arguments_json) const noexcept override {
        script::GcHeap heap(exchange.pool());
        script::JsValue root;
        json::ParseError parse_error;
        if (script::json::decode_js_value(heap, arguments_json.data(), arguments_json.size(), root, &parse_error) !=
            json::DecodeStatus::Complete) {
            co_return McpToolCallResult{
                    .text = "invalid tool arguments JSON",
                    .has_content = true,
                    .is_error = true,
            };
        }
        http_script::ScriptExchangeCtx context(exchange, heap);
        context.set_services(services_);
        script::ScriptResult result = co_await environment_->program.exec_async(root, &context, heap);
        if (result.is_void()) {
            co_return McpToolCallResult{};
        }
        if (result.is_exception()) {
            co_return McpToolCallResult{
                    .text = exception_message(result.exception()),
                    .has_content = true,
                    .is_error = true,
            };
        }
        if (result.is_abort()) {
            std::string message = "script aborted: ";
            message.append(script::abort_reason_name(result.abort().reason));
            co_return McpToolCallResult{
                    .text = std::move(message),
                    .has_content = true,
                    .is_error = true,
            };
        }
        McpToolCallResult output{.has_content = true};
        const script::JsNodeType type = script::js_value_type(result.value());
        if (type == script::JsNodeType::Object || type == script::JsNodeType::Array) {
            class StringSink final : public json::OutputSink {
            public:
                explicit StringSink(std::string &output) noexcept : output_(&output) {}
                bool write(const char *data, std::size_t size) noexcept override {
                    output_->append(data, size);
                    return true;
                }

            private:
                std::string *output_ = nullptr;
            } sink(output.text);
            json::Generator generator(sink);
            const auto encoded = script::json::encode_js_value(generator, result.value());
            if (encoded != json::Generator::Result::OK && encoded != json::Generator::Result::GenerateComplete) {
                output.text = "failed to encode tool result";
                output.is_error = true;
            }
        } else {
            script::std_lib::node_as_text(result.value(), output.text);
        }
        co_return output;
    }

private:
    std::shared_ptr<ScriptEnvironment> environment_;
    http_script::HttpScriptServices *services_ = nullptr;
};

} // namespace

std::expected<std::shared_ptr<const McpToolHandler>, McpScriptCompileError>
compile_mcp_tool_script(std::string_view source, http_script::HttpScriptServices *services) {
    auto *mcp_services = dynamic_cast<McpScriptServices *>(services);
    auto environment = std::make_shared<ScriptEnvironment>(mcp_services);
    auto compiled = script::compile_script(environment->library, source);
    if (!compiled) {
        return std::unexpected(McpScriptCompileError{
                .offset = compiled.error().position,
                .message = std::move(compiled.error().message),
        });
    }
    environment->program = std::move(*compiled);
    return std::static_pointer_cast<const McpToolHandler>(
            std::make_shared<ScriptToolHandler>(std::move(environment), services));
}

} // namespace fiber::ai_server
