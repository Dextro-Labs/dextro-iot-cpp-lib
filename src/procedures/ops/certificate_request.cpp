#include <dextro-iot/procedures/ops/certificate_request.hpp>

namespace dextro::iot::procedures {

CertificateRequest::CertificateRequest(CertificateStore* certificates) : certificates_(certificates)
{
}

std::optional<Reply> CertificateRequest::validate(const Json& payload) const
{
    if (auto refused = only_keys(payload, {"requestId", "reason", "keyType"})) {
        return refused;
    }
    for (const char* key : {"requestId", "reason", "keyType"}) {
        if (auto refused = require(payload, key)) {
            return refused;
        }
        if (auto refused = string_field(payload, key, 64)) {
            return refused;
        }
    }
    if (!is_uuid(payload["requestId"].get_ref<const std::string&>())) {
        return Reply::invalid("requestId não é uuid");
    }
    const auto& reason = payload["reason"].get_ref<const std::string&>();
    if (reason != "initial" && reason != "renewal" && reason != "revoked") {
        return Reply::invalid("reason fora de initial|renewal|revoked");
    }
    if (payload["keyType"].get_ref<const std::string&>() != "EC-P256") {
        return Reply::invalid("keyType não é EC-P256");
    }
    return std::nullopt;
}

Reply CertificateRequest::execute(const Request& request)
{
    if (certificates_ == nullptr) {
        return Reply::error(Rescode::ServerError, ErrorCode::Unavailable, "certificados");
    }

    const auto& request_id = request.payload["requestId"].get_ref<const std::string&>();
    std::string csr;
    switch (certificates_->request_csr(request_id, csr)) {
    case CertificateStore::Result::Ok:
        return Reply::ok({{"requestId", request_id}, {"csrPem", std::move(csr)}});
    case CertificateStore::Result::Storage:
        return Reply::error(Rescode::ServerError, ErrorCode::Storage, "chave pendente");
    case CertificateStore::Result::KeyGeneration:
    case CertificateStore::Result::Mismatch:
    case CertificateStore::Result::Invalid:
        break;
    }
    return Reply::error(Rescode::ServerError, ErrorCode::KeyGeneration, "EC-P256");
}

}  // namespace dextro::iot::procedures
