#include "JsonPath.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <utility>

namespace fiber::json {
namespace {

constexpr std::size_t kMaxPathDepth = 128;

JsonPathCompileError compile_error(JsonPathCompileErrorCode code, std::size_t rule_index, std::size_t offset,
                                   std::string message) {
    return JsonPathCompileError{
            .code = code,
            .rule_index = rule_index,
            .expression_offset = offset,
            .message = std::move(message),
    };
}

bool copy_to_pool(mem::BufPool &pool, std::string_view input, std::string_view &out) noexcept {
    if (input.empty()) {
        out = {};
        return true;
    }
    auto *data = static_cast<char *>(pool.alloc(input.size(), alignof(char)));
    if (!data) {
        return false;
    }
    std::memcpy(data, input.data(), input.size());
    out = std::string_view(data, input.size());
    return true;
}

bool valid_encoded_json_value(std::string_view input) noexcept {
    if (input.empty()) {
        return false;
    }
    JsonParser parser;
    if (!parser.feed(input.data(), input.size())) {
        return false;
    }
    parser.finish();
    if (parser.next() != JsonParser::Status::Token) {
        return false;
    }
    const Token *token = parser.current_token();
    if (!token || token->role != TokenRole::Value) {
        return false;
    }

    std::size_t depth = 0;
    if (token->kind == TokenKind::StartObj || token->kind == TokenKind::StartArr) {
        depth = 1;
    }
    while (depth > 0) {
        if (parser.next() != JsonParser::Status::Token) {
            return false;
        }
        token = parser.current_token();
        if (token->kind == TokenKind::StartObj || token->kind == TokenKind::StartArr) {
            ++depth;
        } else if (token->kind == TokenKind::EndObj || token->kind == TokenKind::EndArr) {
            --depth;
        }
    }
    return parser.next() == JsonParser::Status::Complete;
}

bool append_buffer(mem::IoBufChain &output, mem::IoBuf buffer) noexcept {
    return buffer.readable() == 0 || output.append(std::move(buffer));
}

struct RewriteContext {
    mem::IoBuf *input = nullptr;
    mem::IoBufChain *output = nullptr;
    JsonPathRewriter rewriter;
    std::size_t cursor = 0;
    JsonPathRewriteErrorCode error = JsonPathRewriteErrorCode::HandlerRejected;
    std::uint32_t error_action = 0;

    static bool on_match(void *opaque, const JsonPathMatch &match) noexcept {
        auto &self = *static_cast<RewriteContext *>(opaque);
        JsonPathReplacement replacement;
        if (!self.rewriter.on_match(self.rewriter.context, match, replacement)) {
            self.error = JsonPathRewriteErrorCode::HandlerRejected;
            self.error_action = match.action;
            return false;
        }
        if (!replacement.replace) {
            return true;
        }
        if (match.span.begin < self.cursor || match.span.end < match.span.begin ||
            match.span.end > self.input->readable()) {
            self.error = JsonPathRewriteErrorCode::InvalidSpan;
            self.error_action = match.action;
            return false;
        }
        if (!valid_encoded_json_value(replacement.encoded_value)) {
            self.error = JsonPathRewriteErrorCode::InvalidReplacement;
            self.error_action = match.action;
            return false;
        }
        if (match.span.begin > self.cursor &&
            !append_buffer(*self.output, self.input->retain_slice(self.cursor, match.span.begin - self.cursor))) {
            self.error = JsonPathRewriteErrorCode::OutOfMemory;
            self.error_action = match.action;
            return false;
        }

        mem::IoBuf encoded = mem::IoBuf::allocate(replacement.encoded_value.size());
        if (!encoded) {
            self.error = JsonPathRewriteErrorCode::OutOfMemory;
            self.error_action = match.action;
            return false;
        }
        std::memcpy(encoded.writable_data(), replacement.encoded_value.data(), replacement.encoded_value.size());
        encoded.commit(replacement.encoded_value.size());
        if (!append_buffer(*self.output, std::move(encoded))) {
            self.error = JsonPathRewriteErrorCode::OutOfMemory;
            self.error_action = match.action;
            return false;
        }
        self.cursor = match.span.end;
        return true;
    }
};

} // namespace

class JsonPathWalker {
public:
    JsonPathWalker(const JsonPathProgram &program, std::string_view input, mem::BufPool &pool,
                   JsonPathVisitor visitor) noexcept :
        program_(&program), input_(input), pool_(&pool), visitor_(visitor) {}

    [[nodiscard]] std::expected<void, JsonPathVisitError> run() noexcept {
        if (!visitor_.on_match) {
            return std::unexpected(JsonPathVisitError{.code = JsonPathVisitErrorCode::InvalidVisitor});
        }

        if (program_->max_capture_depth_ > 0) {
            captures_ = pool_->alloc<JsonPathCapture>(program_->max_capture_depth_);
            if (!captures_) {
                return std::unexpected(JsonPathVisitError{.code = JsonPathVisitErrorCode::OutOfMemory});
            }
        }

        if (!parser_.feed(input_.data(), input_.size())) {
            return invalid_json();
        }
        parser_.finish();
        if (!advance("expected a JSON document")) {
            return std::unexpected(error_);
        }
        if (!process_value(0)) {
            return std::unexpected(error_);
        }

        switch (parser_.next()) {
            case JsonParser::Status::Complete:
                return {};
            case JsonParser::Status::Error:
                return invalid_json();
            case JsonParser::Status::Token:
                (void) parser_.fail("multiple JSON root values");
                return invalid_json();
            case JsonParser::Status::NeedMore:
                (void) parser_.fail("unexpected end of JSON input");
                return invalid_json();
        }
        (void) parser_.fail("invalid parser state");
        return invalid_json();
    }

private:
    [[nodiscard]] std::unexpected<JsonPathVisitError> invalid_json() noexcept {
        return std::unexpected(JsonPathVisitError{
                .code = JsonPathVisitErrorCode::InvalidJson,
                .parse_error = parser_.error(),
        });
    }

    [[nodiscard]] bool fail(JsonPathVisitErrorCode code, std::uint32_t action = 0) noexcept {
        error_.code = code;
        error_.parse_error = parser_.error();
        error_.action = action;
        return false;
    }

    [[nodiscard]] bool advance(const char *message) noexcept {
        switch (parser_.next()) {
            case JsonParser::Status::Token:
                return true;
            case JsonParser::Status::Error:
                return fail(JsonPathVisitErrorCode::InvalidJson);
            case JsonParser::Status::Complete:
            case JsonParser::Status::NeedMore:
                (void) parser_.fail(message);
                return fail(JsonPathVisitErrorCode::InvalidJson);
        }
        (void) parser_.fail("invalid parser state");
        return fail(JsonPathVisitErrorCode::InvalidJson);
    }

    [[nodiscard]] bool push_capture(const std::string &name, JsonPathCaptureKind kind, std::string_view text,
                                    std::size_t index) noexcept {
        if (name.empty()) {
            return true;
        }
        if (capture_size_ >= program_->max_capture_depth_) {
            return fail(JsonPathVisitErrorCode::OutOfMemory);
        }
        std::string_view owned_text;
        if (!copy_to_pool(*pool_, text, owned_text)) {
            return fail(JsonPathVisitErrorCode::OutOfMemory);
        }
        captures_[capture_size_++] = JsonPathCapture{
                .name = name,
                .text = owned_text,
                .index = index,
                .kind = kind,
        };
        return true;
    }

    void pop_capture(const std::string &name) noexcept {
        if (!name.empty()) {
            --capture_size_;
        }
    }

    [[nodiscard]] bool push_index_capture(const std::string &name, std::size_t index) noexcept {
        if (name.empty()) {
            return true;
        }
        std::array<char, 32> buffer{};
        auto conversion = std::to_chars(buffer.data(), buffer.data() + buffer.size(), index);
        if (conversion.ec != std::errc()) {
            return fail(JsonPathVisitErrorCode::OutOfMemory);
        }
        return push_capture(name, JsonPathCaptureKind::ArrayIndex,
                            std::string_view(buffer.data(), static_cast<std::size_t>(conversion.ptr - buffer.data())),
                            index);
    }

    [[nodiscard]] bool dispatch(std::uint32_t action, const Token &token, JsonValueSpan span) noexcept {
        JsonPathMatch match{
                .action = action,
                .token = token,
                .span = span,
                .variables = JsonPathVarScope(std::span<const JsonPathCapture>(captures_, capture_size_)),
        };
        if (!visitor_.on_match(visitor_.context, match)) {
            return fail(JsonPathVisitErrorCode::HandlerRejected, action);
        }
        return true;
    }

    [[nodiscard]] bool skip_current_value() noexcept {
        const Token *token = parser_.current_token();
        if (!token || token->role != TokenRole::Value) {
            (void) parser_.fail("expected JSON value");
            return fail(JsonPathVisitErrorCode::InvalidJson);
        }
        if (token->kind != TokenKind::StartObj && token->kind != TokenKind::StartArr) {
            return true;
        }

        std::size_t depth = 1;
        while (depth > 0) {
            if (!advance("unexpected end of JSON value")) {
                return false;
            }
            token = parser_.current_token();
            if (token->kind == TokenKind::StartObj || token->kind == TokenKind::StartArr) {
                ++depth;
            } else if (token->kind == TokenKind::EndObj || token->kind == TokenKind::EndArr) {
                --depth;
            }
        }
        return true;
    }

    [[nodiscard]] bool process_value(std::uint32_t node_index) noexcept {
        const auto &node = program_->nodes_[node_index];
        const Token *current = parser_.current_token();
        if (!current || current->role != TokenRole::Value) {
            (void) parser_.fail("expected JSON value");
            return fail(JsonPathVisitErrorCode::InvalidJson);
        }

        const Token value_token = *current;
        const std::size_t begin = parser_.current_offset();
        if (node.action) {
            if (!skip_current_value()) {
                return false;
            }
            return dispatch(*node.action, value_token,
                            JsonValueSpan{.begin = begin, .end = parser_.current_end_offset()});
        }

        if (value_token.kind == TokenKind::StartObj) {
            return process_object(node_index);
        }
        if (value_token.kind == TokenKind::StartArr) {
            return process_array(node_index);
        }
        // A scalar at an intermediate path is simply a non-match. The caller
        // still advances normally, so the complete document remains validated.
        return true;
    }

    [[nodiscard]] bool process_object(std::uint32_t node_index) noexcept {
        const auto &node = program_->nodes_[node_index];
        if (!advance("unexpected end of JSON object")) {
            return false;
        }

        while (parser_.current_token()->kind != TokenKind::EndObj) {
            const Token *key_token = parser_.current_token();
            if (key_token->kind != TokenKind::Text || key_token->role != TokenRole::ObjectKey) {
                (void) parser_.fail("expected object key");
                return fail(JsonPathVisitErrorCode::InvalidJson);
            }

            const std::string_view key = key_token->view;
            std::uint32_t child = program_->find_exact(node_index, key);
            bool captured = false;
            if (child == JsonPathProgram::NoNode && node.wildcard_key != JsonPathProgram::NoNode) {
                child = node.wildcard_key;
                const auto &wildcard = program_->nodes_[child];
                if (!push_capture(wildcard.capture_name, JsonPathCaptureKind::ObjectKey, key, 0)) {
                    return false;
                }
                captured = !wildcard.capture_name.empty();
            }

            if (!advance("object key without value")) {
                return false;
            }
            const bool processed = child == JsonPathProgram::NoNode ? skip_current_value() : process_value(child);
            if (captured) {
                pop_capture(program_->nodes_[child].capture_name);
            }
            if (!processed || !advance("unexpected end of JSON object")) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool process_array(std::uint32_t node_index) noexcept {
        const auto &node = program_->nodes_[node_index];
        std::size_t index = 0;
        if (!advance("unexpected end of JSON array")) {
            return false;
        }

        while (parser_.current_token()->kind != TokenKind::EndArr) {
            std::uint32_t child = program_->find_index(node_index, index);
            bool captured = false;
            if (child == JsonPathProgram::NoNode && node.wildcard_index != JsonPathProgram::NoNode) {
                child = node.wildcard_index;
                const auto &wildcard = program_->nodes_[child];
                if (!push_index_capture(wildcard.capture_name, index)) {
                    return false;
                }
                captured = !wildcard.capture_name.empty();
            }

            const bool processed = child == JsonPathProgram::NoNode ? skip_current_value() : process_value(child);
            if (captured) {
                pop_capture(program_->nodes_[child].capture_name);
            }
            if (!processed) {
                return false;
            }
            if (index != std::numeric_limits<std::size_t>::max()) {
                ++index;
            }
            if (!advance("unexpected end of JSON array")) {
                return false;
            }
        }
        return true;
    }

    const JsonPathProgram *program_ = nullptr;
    std::string_view input_;
    mem::BufPool *pool_ = nullptr;
    JsonPathVisitor visitor_;
    JsonParser parser_;
    JsonPathCapture *captures_ = nullptr;
    std::size_t capture_size_ = 0;
    JsonPathVisitError error_;
};

bool JsonPathProgram::Node::has_children() const noexcept {
    return !exact.empty() || !indices.empty() || wildcard_key != NoNode || wildcard_index != NoNode;
}

std::uint32_t JsonPathProgram::add_node() {
    nodes_.emplace_back();
    return static_cast<std::uint32_t>(nodes_.size() - 1);
}

std::uint32_t JsonPathProgram::find_exact(std::uint32_t node, std::string_view key) const noexcept {
    for (const ExactEdge &edge: nodes_[node].exact) {
        if (edge.key == key) {
            return edge.child;
        }
    }
    return NoNode;
}

std::uint32_t JsonPathProgram::find_index(std::uint32_t node, std::size_t index) const noexcept {
    if (index > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        return NoNode;
    }
    const std::int32_t value = static_cast<std::int32_t>(index);
    const auto &indices = nodes_[node].indices;
    const auto it =
            std::lower_bound(indices.begin(), indices.end(), value,
                             [](const IndexEdge &edge, std::int32_t expected) { return edge.index < expected; });
    return it != indices.end() && it->index == value ? it->child : NoNode;
}

bool JsonPathProgram::subtree_active(std::uint32_t node) const noexcept {
    if (node == NoNode) {
        return false;
    }
    const Node &value = nodes_[node];
    if (value.action) {
        return true;
    }
    for (const ExactEdge &edge: value.exact) {
        if (subtree_active(edge.child)) {
            return true;
        }
    }
    for (const IndexEdge &edge: value.indices) {
        if (subtree_active(edge.child)) {
            return true;
        }
    }
    return subtree_active(value.wildcard_key) || subtree_active(value.wildcard_index);
}

std::expected<JsonPathProgram, JsonPathCompileError> JsonPathProgram::compile(std::span<const JsonPathRule> rules) {
    JsonPathProgram program;
    (void) program.add_node();

    for (std::size_t rule_index = 0; rule_index < rules.size(); ++rule_index) {
        const JsonPathRule &rule = rules[rule_index];
        const std::string_view expression = rule.expression;
        if (expression.empty() || expression.front() != '$') {
            return std::unexpected(compile_error(JsonPathCompileErrorCode::InvalidExpression, rule_index, 0,
                                                 "expression must start with '$'"));
        }

        std::size_t pos = 1;
        std::size_t depth = 0;
        std::size_t capture_depth = 0;
        std::uint32_t current = 0;
        while (pos < expression.size()) {
            if (++depth > kMaxPathDepth) {
                return std::unexpected(compile_error(JsonPathCompileErrorCode::PathTooDeep, rule_index, pos,
                                                     "JSON path exceeds maximum depth"));
            }
            if (program.nodes_[current].action) {
                return std::unexpected(compile_error(JsonPathCompileErrorCode::PrefixConflict, rule_index, pos,
                                                     "path passes through an existing terminal path"));
            }

            if (expression[pos] == '.') {
                const std::size_t segment_offset = pos++;
                if (pos == expression.size()) {
                    return std::unexpected(compile_error(JsonPathCompileErrorCode::InvalidExpression, rule_index,
                                                         segment_offset, "field segment is missing"));
                }

                if (expression[pos] == '*') {
                    ++pos;
                    const std::size_t capture_begin = pos;
                    while (pos < expression.size() && expression[pos] != '.' && expression[pos] != '[') {
                        ++pos;
                    }
                    const std::string_view capture = expression.substr(capture_begin, pos - capture_begin);

                    for (const ExactEdge &edge: program.nodes_[current].exact) {
                        if (program.subtree_active(edge.child)) {
                            return std::unexpected(compile_error(JsonPathCompileErrorCode::WildcardConflict, rule_index,
                                                                 segment_offset,
                                                                 "object wildcard conflicts with an exact field"));
                        }
                    }

                    std::uint32_t child = program.nodes_[current].wildcard_key;
                    if (child == NoNode) {
                        child = program.add_node();
                        program.nodes_[current].wildcard_key = child;
                        program.nodes_[child].capture_name.assign(capture);
                    } else if (program.nodes_[child].capture_name != capture) {
                        return std::unexpected(compile_error(JsonPathCompileErrorCode::CaptureConflict, rule_index,
                                                             capture_begin,
                                                             "wildcard capture name conflicts with an existing path"));
                    }
                    if (!capture.empty()) {
                        ++capture_depth;
                    }
                    current = child;
                    continue;
                }

                const std::size_t field_begin = pos;
                while (pos < expression.size() && expression[pos] != '.' && expression[pos] != '[') {
                    ++pos;
                }
                if (pos == field_begin) {
                    return std::unexpected(compile_error(JsonPathCompileErrorCode::InvalidExpression, rule_index,
                                                         field_begin, "field name is empty"));
                }
                if (program.subtree_active(program.nodes_[current].wildcard_key)) {
                    return std::unexpected(compile_error(JsonPathCompileErrorCode::WildcardConflict, rule_index,
                                                         field_begin, "exact field conflicts with an object wildcard"));
                }

                const std::string_view field = expression.substr(field_begin, pos - field_begin);
                std::uint32_t child = program.find_exact(current, field);
                if (child == NoNode) {
                    child = program.add_node();
                    program.nodes_[current].exact.push_back(ExactEdge{
                            .key = std::string(field),
                            .child = child,
                    });
                }
                current = child;
                continue;
            }

            if (expression[pos] == '[') {
                const std::size_t segment_offset = pos++;
                const std::size_t value_begin = pos;
                while (pos < expression.size() && expression[pos] != ']') {
                    ++pos;
                }
                if (pos == expression.size()) {
                    return std::unexpected(compile_error(JsonPathCompileErrorCode::InvalidExpression, rule_index,
                                                         segment_offset, "array segment is not closed"));
                }
                const std::string_view value = expression.substr(value_begin, pos - value_begin);
                ++pos;
                if (value.empty()) {
                    return std::unexpected(compile_error(JsonPathCompileErrorCode::InvalidExpression, rule_index,
                                                         value_begin, "array index is empty"));
                }

                if (value.front() == '*') {
                    const std::string_view capture = value.substr(1);
                    for (const IndexEdge &edge: program.nodes_[current].indices) {
                        if (program.subtree_active(edge.child)) {
                            return std::unexpected(compile_error(JsonPathCompileErrorCode::WildcardConflict, rule_index,
                                                                 segment_offset,
                                                                 "array wildcard conflicts with an exact index"));
                        }
                    }

                    std::uint32_t child = program.nodes_[current].wildcard_index;
                    if (child == NoNode) {
                        child = program.add_node();
                        program.nodes_[current].wildcard_index = child;
                        program.nodes_[child].capture_name.assign(capture);
                    } else if (program.nodes_[child].capture_name != capture) {
                        return std::unexpected(compile_error(JsonPathCompileErrorCode::CaptureConflict, rule_index,
                                                             value_begin + 1,
                                                             "wildcard capture name conflicts with an existing path"));
                    }
                    if (!capture.empty()) {
                        ++capture_depth;
                    }
                    current = child;
                    continue;
                }

                std::int32_t index = 0;
                const auto conversion = std::from_chars(value.data(), value.data() + value.size(), index);
                if (conversion.ec != std::errc() || conversion.ptr != value.data() + value.size() || index < 0) {
                    return std::unexpected(compile_error(JsonPathCompileErrorCode::InvalidExpression, rule_index,
                                                         value_begin, "array index must be non-negative"));
                }
                if (program.subtree_active(program.nodes_[current].wildcard_index)) {
                    return std::unexpected(compile_error(JsonPathCompileErrorCode::WildcardConflict, rule_index,
                                                         value_begin, "exact index conflicts with an array wildcard"));
                }

                std::uint32_t child = program.find_index(current, static_cast<std::size_t>(index));
                if (child == NoNode) {
                    child = program.add_node();
                    auto &indices = program.nodes_[current].indices;
                    const auto at = std::lower_bound(
                            indices.begin(), indices.end(), index,
                            [](const IndexEdge &edge, std::int32_t expected) { return edge.index < expected; });
                    indices.insert(at, IndexEdge{.index = index, .child = child});
                }
                current = child;
                continue;
            }

            return std::unexpected(compile_error(JsonPathCompileErrorCode::InvalidExpression, rule_index, pos,
                                                 "unexpected character in JSON path"));
        }

        Node &leaf = program.nodes_[current];
        if (leaf.action) {
            return std::unexpected(compile_error(JsonPathCompileErrorCode::DuplicatePath, rule_index, pos,
                                                 "path is already registered"));
        }
        if (leaf.has_children()) {
            return std::unexpected(compile_error(JsonPathCompileErrorCode::PrefixConflict, rule_index, pos,
                                                 "path is a prefix of an existing path"));
        }
        leaf.action = rule.action;
        ++program.rule_count_;
        program.max_capture_depth_ = std::max(program.max_capture_depth_, capture_depth);
    }

    return program;
}

bool JsonPathProgram::empty() const noexcept { return rule_count_ == 0; }

std::size_t JsonPathProgram::rule_count() const noexcept { return rule_count_; }

std::size_t JsonPathProgram::max_capture_depth() const noexcept { return max_capture_depth_; }

const JsonPathCapture *JsonPathVarScope::find(std::string_view name) const noexcept {
    for (std::size_t i = captures_.size(); i > 0; --i) {
        if (captures_[i - 1].name == name) {
            return &captures_[i - 1];
        }
    }
    return nullptr;
}

std::string_view JsonPathVarScope::get(std::string_view name) const noexcept {
    const JsonPathCapture *capture = find(name);
    return capture ? capture->text : std::string_view{};
}

std::expected<void, JsonPathVisitError> visit_json_paths(const JsonPathProgram &program, std::string_view input,
                                                         mem::BufPool &pool, JsonPathVisitor visitor) noexcept {
    return JsonPathWalker(program, input, pool, visitor).run();
}

std::expected<mem::IoBufChain, JsonPathRewriteError> rewrite_json_paths(const JsonPathProgram &program,
                                                                        mem::IoBuf input, mem::BufPool &pool,
                                                                        mem::IoBufNodePool &node_pool,
                                                                        JsonPathRewriter rewriter) noexcept {
    if (!rewriter.on_match) {
        return std::unexpected(JsonPathRewriteError{
                .code = JsonPathRewriteErrorCode::InvalidRewriter,
        });
    }
    if (!input) {
        return std::unexpected(JsonPathRewriteError{
                .code = JsonPathRewriteErrorCode::InvalidJson,
        });
    }

    mem::IoBufChain output(node_pool);
    RewriteContext context{
            .input = &input,
            .output = &output,
            .rewriter = rewriter,
    };
    const std::string_view text(reinterpret_cast<const char *>(input.readable_data()), input.readable());
    auto visited = visit_json_paths(program, text, pool,
                                    JsonPathVisitor{
                                            .context = &context,
                                            .on_match = &RewriteContext::on_match,
                                    });
    if (!visited) {
        JsonPathRewriteErrorCode code = JsonPathRewriteErrorCode::InvalidJson;
        if (visited.error().code == JsonPathVisitErrorCode::OutOfMemory) {
            code = JsonPathRewriteErrorCode::OutOfMemory;
        } else if (visited.error().code == JsonPathVisitErrorCode::HandlerRejected) {
            code = context.error;
        }
        return std::unexpected(JsonPathRewriteError{
                .code = code,
                .visit_error = visited.error(),
                .action = context.error_action,
        });
    }
    if (context.cursor < input.readable() &&
        !append_buffer(output, input.retain_slice(context.cursor, input.readable() - context.cursor))) {
        return std::unexpected(JsonPathRewriteError{
                .code = JsonPathRewriteErrorCode::OutOfMemory,
        });
    }
    output.mark_complete();
    return output;
}

} // namespace fiber::json
