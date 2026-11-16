#pragma once

#include <dextro-iot/procedures/procedure_table.hpp>

/// `outbox-notification` (spec §6, contracts.md A.4.4).
///
/// Só um aviso: "tem coisa na outbox". A op marca a drenagem e responde 204.
/// Quem drena é a aplicação (`SessionControl::request_outbox_drain`), pelas remote-procedures `outbox-read` e
/// `outbox-ack` — e ela drena também no connect e periodicamente, porque o
/// aviso é best-effort e pode se perder (spec §6.5).
namespace dextro::iot::procedures {

class OutboxNotification final : public Procedure {
public:
    explicit OutboxNotification(SessionControl* session);

    std::optional<Reply> validate(const Json& payload) const override;
    Reply execute(const Request& request) override;

private:
    SessionControl* session_;
};

}  // namespace dextro::iot::procedures
