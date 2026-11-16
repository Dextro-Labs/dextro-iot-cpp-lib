#pragma once

#include <dextro-iot/procedures/procedure_table.hpp>

/// `list-procedures` (spec §5, contracts.md A.4.1).
///
/// O backend grava a resposta em `device.capabilities` e usa a `version` de
/// cada op para escolher o schema. Por isso ela sai da tabela, e não de uma
/// lista escrita à mão: uma op acrescentada à tabela aparece aqui sozinha.
namespace dextro::iot::procedures {

class ListProcedures final : public Procedure {
public:
    /// `device_type`: o tipo de device, como está cadastrado no
    /// dextrolabs-device (`"ssat-cta4000"`, `"allprint-..."`).
    /// `contract_version`: a versão do arquivo de procedures do tipo que este
    /// firmware implementa. A tabela tem que viver mais que a op — e ela vive:
    /// a op é registrada nela.
    ListProcedures(const ProcedureTable& table, std::string device_type, int contract_version);

    std::optional<Reply> validate(const Json& payload) const override;
    Reply execute(const Request& request) override;

private:
    const ProcedureTable& table_;
    std::string device_type_;
    int contract_version_;
};

}  // namespace dextro::iot::procedures
