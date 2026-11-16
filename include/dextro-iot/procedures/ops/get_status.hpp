#pragma once

#include <dextro-iot/procedures/procedure_table.hpp>

/// `get-status` (spec §5, contracts.md A.4.2).
///
/// `sections` ausente vale `["all"]`. Uma seção que o device não conhece é
/// 400: o operador que pediu `"enlace"` em vez de `"link"` precisa saber que
/// errou, e não receber um objeto vazio que parece "está tudo bem".
namespace dextro::iot::procedures {

class GetStatus final : public Procedure {
public:
    explicit GetStatus(const StatusSource* status);

    std::optional<Reply> validate(const Json& payload) const override;
    Reply execute(const Request& request) override;

private:
    const StatusSource* status_;
};

}  // namespace dextro::iot::procedures
