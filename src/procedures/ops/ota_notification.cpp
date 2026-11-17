#include <dextro-iot/procedures/ops/ota_notification.hpp>

#include <algorithm>

namespace dextro::iot::procedures {
namespace {

bool contains(const std::vector<std::string>& list, std::string_view value)
{
    return std::find(list.begin(), list.end(), value) != list.end();
}

}  // namespace

bool OtaNotification::is_ed25519_base64(std::string_view text) noexcept
{
    // Uma assinatura ed25519 tem 64 bytes; em base64 com padding, 88
    // caracteres terminando em `==`. Conferir a FORMA aqui poupa o agente de
    // baixar um artefato cuja assinatura nem é uma assinatura.
    if (text.size() != 88 || text.substr(86) != "==") {
        return false;
    }
    return std::all_of(text.begin(), text.begin() + 86, [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '+' || c == '/';
    });
}

OtaNotification::OtaNotification(OtaSpool* spool, const Keyring* keyring,
                                 const InstalledVersion* installed)
    : OtaNotification(spool, keyring, installed, Options{})
{
}

OtaNotification::OtaNotification(OtaSpool* spool, const Keyring* keyring,
                                 const InstalledVersion* installed, Options options)
    : spool_(spool), keyring_(keyring), installed_(installed), options_(std::move(options))
{
}

std::optional<Reply> OtaNotification::validate(const Json& payload) const
{
    if (auto refused = only_keys(payload, {"version", "artifactUrl", "sha256", "signature",
                                           "signingKeyId", "sizeBytes", "artifactFormat",
                                           "applyAfter", "campaignItemId", "progressEntityId",
                                           "artifactName"})) {
        return refused;
    }
    for (const char* key : {"version", "artifactUrl", "sha256", "campaignItemId"}) {
        if (auto refused = require(payload, key)) {
            return refused;
        }
        if (auto refused = string_field(payload, key, 2048)) {
            return refused;
        }
    }
    if (auto refused = string_field(payload, "signature", 2048)) {
        return refused;
    }
    if (auto refused = string_field(payload, "signingKeyId", 64)) {
        return refused;
    }
    if (auto refused = string_field(payload, "progressEntityId", 256)) {
        return refused;
    }
    if (auto refused = string_field(payload, "artifactName", 64)) {
        return refused;
    }
    if (auto refused = integer_field(payload, "sizeBytes", 1)) {
        return refused;
    }
    if (auto refused = integer_field(payload, "applyAfter", 0)) {
        return refused;
    }

    std::string format;
    if (const auto it = payload.find("artifactFormat"); it != payload.end()) {
        if (!it->is_string() || !contains(options_.formats, it->get_ref<const std::string&>())) {
            return Reply::invalid("artifactFormat fora dos formatos aceitos");
        }
        format = it->get<std::string>();
    }
    if (!is_version_token(payload["version"].get_ref<const std::string&>())) {
        return Reply::invalid("version fora de [A-Za-z0-9._+-]{1,64}");
    }
    // O artefato trafega FORA do MQTT, e só por HTTPS.
    if (payload["artifactUrl"].get_ref<const std::string&>().rfind("https://", 0) != 0) {
        return Reply::invalid("artifactUrl não é https://");
    }
    if (!is_sha256_hex(payload["sha256"].get_ref<const std::string&>())) {
        return Reply::error(Rescode::BadRequest, ErrorCode::ChecksumInvalid,
                            "sha256 não é hex de 64");
    }
    // Obrigatória fora dos formatos que se assinam sozinhos (o `if` do schema
    // de A.4.3). Presente, a forma é conferida em qualquer formato.
    const auto signature = payload.find("signature");
    if (signature == payload.end()) {
        if (!contains(options_.self_signed, format)) {
            return Reply::error(Rescode::BadRequest, ErrorCode::SignatureInvalid,
                                "signature obrigatória para o formato '" + format + "'");
        }
    } else if (!is_ed25519_base64(signature->get_ref<const std::string&>())) {
        return Reply::error(Rescode::BadRequest, ErrorCode::SignatureInvalid,
                            "signature não é ed25519 em base64");
    }
    if (!is_uuid(payload["campaignItemId"].get_ref<const std::string&>())) {
        return Reply::invalid("campaignItemId não é uuid");
    }
    return std::nullopt;
}

Reply OtaNotification::execute(const Request& request)
{
    if (spool_ == nullptr) {
        return Reply::error(Rescode::ServerError, ErrorCode::Unavailable, "ota");
    }
    const Json& payload = request.payload;

    if (installed_ != nullptr) {
        const auto current = installed_->installed_version(payload);
        if (current && *current == payload["version"].get_ref<const std::string&>()) {
            return Reply::ok({{"alreadyInstalled", true}});
        }
    }

    const std::string key = payload.value("signingKeyId", std::string{});
    if (!key.empty() && keyring_ != nullptr && !keyring_->known(key)) {
        return Reply::error(Rescode::BadRequest, ErrorCode::UnknownSigningKey, key);
    }

    if (!spool_->enqueue(payload)) {
        return Reply::error(Rescode::ServerError, ErrorCode::Storage, "spool do OTA");
    }
    return Reply::accepted();
}

}  // namespace dextro::iot::procedures
