#pragma once

#include <dextro-iot/procedures/procedure_table.hpp>

/// `certificate-request` (SPEC do dextrolabs-device §3.5, contracts.md A.4.7).
///
/// O device gera um par EC P-256 novo e devolve a CSR. A chave privada nunca
/// sai do device. Atendida também no escopo bootstrap: é justamente com o
/// certificado de fábrica que o primeiro operacional é pedido.
namespace dextro::iot::procedures {

class CertificateRequest final : public Procedure {
public:
    explicit CertificateRequest(CertificateStore* certificates);

    std::optional<Reply> validate(const Json& payload) const override;
    Reply execute(const Request& request) override;

private:
    CertificateStore* certificates_;
};

}  // namespace dextro::iot::procedures
