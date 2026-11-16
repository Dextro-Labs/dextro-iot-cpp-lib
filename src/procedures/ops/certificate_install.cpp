#include <dextro-iot/procedures/ops/certificate_install.hpp>

namespace dextro::iot::procedures {

CertificateInstall::CertificateInstall(CertificateStore* certificates, SessionControl* session)
    : certificates_(certificates), session_(session)
{
}

std::optional<Reply> CertificateInstall::validate(const Json& payload) const
{
    if (auto refused = only_keys(payload, {"certificateId", "certPem", "chainPem", "notAfter"})) {
        return refused;
    }
    for (const char* key : {"certificateId", "certPem", "chainPem", "notAfter"}) {
        if (auto refused = require(payload, key)) {
            return refused;
        }
    }
    if (auto refused = string_field(payload, "certificateId", 64)) {
        return refused;
    }
    if (auto refused = string_field(payload, "certPem", 8192)) {
        return refused;
    }
    if (auto refused = string_field(payload, "chainPem", 16384)) {
        return refused;
    }
    if (auto refused = integer_field(payload, "notAfter", 0)) {
        return refused;
    }
    if (!is_uuid(payload["certificateId"].get_ref<const std::string&>())) {
        return Reply::invalid("certificateId não é uuid");
    }
    return std::nullopt;
}

Reply CertificateInstall::execute(const Request& request)
{
    if (certificates_ == nullptr) {
        return Reply::error(Rescode::ServerError, ErrorCode::Unavailable, "certificados");
    }

    std::string fingerprint;
    switch (certificates_->install(request.payload["certPem"].get_ref<const std::string&>(),
                                   request.payload["chainPem"].get_ref<const std::string&>(),
                                   fingerprint)) {
    case CertificateStore::Result::Ok:
        break;
    case CertificateStore::Result::Mismatch:
        return Reply::error(Rescode::BadRequest, ErrorCode::CsrInvalid,
                            "o certificado não casa com nenhuma chave pendente");
    case CertificateStore::Result::Invalid:
        return Reply::error(Rescode::BadRequest, ErrorCode::CsrInvalid, "PEM ilegível");
    case CertificateStore::Result::Storage:
    case CertificateStore::Result::KeyGeneration:
        return Reply::error(Rescode::ServerError, ErrorCode::Storage, "certificado");
    }

    if (session_ != nullptr) {
        session_->request_reconnect();
    }
    return Reply::ok({{"installed", true}, {"fingerprint", std::move(fingerprint)}});
}

}  // namespace dextro::iot::procedures
