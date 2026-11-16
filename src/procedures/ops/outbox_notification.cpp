#include <dextro-iot/procedures/ops/outbox_notification.hpp>

namespace dextro::iot::procedures {

OutboxNotification::OutboxNotification(SessionControl* session) : session_(session)
{
}

std::optional<Reply> OutboxNotification::validate(const Json& payload) const
{
    if (auto refused = only_keys(payload, {"entities", "size"})) {
        return refused;
    }
    if (auto refused = require(payload, "entities")) {
        return refused;
    }
    if (auto refused = require(payload, "size")) {
        return refused;
    }
    const Json& entities = payload["entities"];
    if (!entities.is_array()) {
        return Reply::invalid("entities não é lista");
    }
    for (const Json& entity : entities) {
        if (!entity.is_string()) {
            return Reply::invalid("entities tem item que não é texto");
        }
    }
    return integer_field(payload, "size", 0);
}

Reply OutboxNotification::execute(const Request& request)
{
    (void)request;
    if (session_ == nullptr) {
        return Reply::error(Rescode::ServerError, ErrorCode::Unavailable, "sessão");
    }
    session_->request_outbox_drain();
    return Reply::no_content();
}

}  // namespace dextro::iot::procedures
