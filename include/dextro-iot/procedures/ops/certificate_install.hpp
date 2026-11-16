#pragma once

#include <dextro-iot/procedures/procedure_table.hpp>

/// `certificate-install` (SPEC do dextrolabs-device §3.5, contracts.md A.4.8).
///
/// Grava o certificado emitido para a chave pendente, responde com o
/// fingerprint e pede a reconexão. **A reconexão acontece depois da
/// resposta**: o backend só marca o certificado como `active` quando o
/// primeiro `keepalive` da sessão nova traz o fingerprint, e para isso ele
/// precisa ter recebido este reply.
namespace dextro::iot::procedures {

class CertificateInstall final : public Procedure {
public:
    /// `session` pode ser nulo: o certificado é instalado e ninguém reconecta.
    CertificateInstall(CertificateStore* certificates, SessionControl* session);

    std::optional<Reply> validate(const Json& payload) const override;
    Reply execute(const Request& request) override;

private:
    CertificateStore* certificates_;
    SessionControl* session_;
};

}  // namespace dextro::iot::procedures
