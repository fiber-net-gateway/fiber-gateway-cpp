#include "ModelAuthorization.h"

namespace fiber::ai_server {

bool valid_llm_model_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    for (const unsigned char ch: name) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
            ch == '-' || ch == '.') {
            continue;
        }
        return false;
    }
    return true;
}

std::expected<AuthorizedModel, ModelAuthorizationError>
authorize_model(const LlmConfigSnapshot &config, std::string_view username, std::string_view requested_model) noexcept {
    if (requested_model.empty()) {
        return std::unexpected(ModelAuthorizationError{
                .code = ModelAuthorizationErrorCode::ModelRequired,
                .message = "model is required",
        });
    }
    if (!valid_llm_model_name(requested_model)) {
        return std::unexpected(ModelAuthorizationError{
                .code = ModelAuthorizationErrorCode::InvalidModelName,
                .message = "invalid model name",
        });
    }
    if (!config.project) {
        return std::unexpected(ModelAuthorizationError{
                .code = ModelAuthorizationErrorCode::ModelConfigUnavailable,
                .message = "model authorization config is unavailable",
        });
    }

    const CompiledModelRoute *route = config.project->find_model(requested_model);
    if (!route) {
        return std::unexpected(ModelAuthorizationError{
                .code = ModelAuthorizationErrorCode::ModelNotAvailable,
                .message = "model is not available",
        });
    }
    if (!route->allow_user_groups.empty()) {
        bool allowed = false;
        for (const auto &group: route->allow_user_groups) {
            if (group && group->contains(username)) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            return std::unexpected(ModelAuthorizationError{
                    .code = ModelAuthorizationErrorCode::ModelNotAvailable,
                    .message = "model is not available",
            });
        }
    }
    return AuthorizedModel{
            .model_name = route->model_name,
            .route = route,
    };
}

} // namespace fiber::ai_server
