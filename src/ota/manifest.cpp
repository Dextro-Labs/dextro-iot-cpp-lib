#include <dextro-iot/ota/manifest.hpp>

#include <dextro-iot/procedures/ota_adapters.hpp>
#include <dextro-iot/procedures/procedure.hpp>

#include <algorithm>
#include <cctype>

namespace dextro::iot::ota {
namespace {

using Json = nlohmann::json;

bool string_at(const Json& j, const char* key, std::string& out, bool required)
{
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return !required;
    }
    if (!it->is_string()) {
        return false;
    }
    out = it->get<std::string>();
    return !required || !out.empty();
}

bool integer_at(const Json& j, const char* key, std::int64_t& out)
{
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return true;
    }
    if (!it->is_number_integer() || it->get<std::int64_t>() < 0) {
        return false;
    }
    out = it->get<std::int64_t>();
    return true;
}

}  // namespace

bool is_safe_id(std::string_view id) noexcept
{
    return procedures::is_safe_file_id(id);
}

std::optional<Manifest> Manifest::parse(std::string_view text, std::string& why,
                                        std::string* id_out)
{
    const Json j = Json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) {
        why = "não é um objeto JSON";
        return std::nullopt;
    }
    Manifest m;
    if (!string_at(j, "campaignItemId", m.campaign_item_id, true) ||
        !is_safe_id(m.campaign_item_id)) {
        why = "campaignItemId ausente ou fora de [0-9A-Za-z_-]{1,64}";
        return std::nullopt;
    }
    if (id_out != nullptr) {
        *id_out = m.campaign_item_id;
    }
    if (!string_at(j, "version", m.version, true) || !procedures::is_version_token(m.version)) {
        why = "version ausente ou fora de [A-Za-z0-9._+-]{1,64}";
        return std::nullopt;
    }
    if (!string_at(j, "artifactUrl", m.artifact_url, true) ||
        m.artifact_url.rfind("https://", 0) != 0) {
        why = "artifactUrl ausente ou não é https://";
        return std::nullopt;
    }
    if (!string_at(j, "sha256", m.sha256, true) || !procedures::is_sha256_hex(m.sha256)) {
        why = "sha256 ausente ou não é hex de 64";
        return std::nullopt;
    }
    std::transform(m.sha256.begin(), m.sha256.end(), m.sha256.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    if (!string_at(j, "artifactFormat", m.artifact_format, false) ||
        !string_at(j, "artifactName", m.artifact_name, false) ||
        !string_at(j, "progressEntityId", m.progress_entity_id, false) ||
        !string_at(j, "signature", m.signature, false) ||
        !string_at(j, "signingKeyId", m.signing_key_id, false)) {
        why = "artifactFormat/artifactName/progressEntityId/signature/signingKeyId não é texto";
        return std::nullopt;
    }
    if (!integer_at(j, "sizeBytes", m.size_bytes) || !integer_at(j, "applyAfter", m.apply_after_ms)) {
        why = "sizeBytes/applyAfter não é inteiro >= 0";
        return std::nullopt;
    }
    return m;
}

nlohmann::json Manifest::to_json() const
{
    Json j = {{"campaignItemId", campaign_item_id},
              {"version", version},
              {"artifactUrl", artifact_url},
              {"sha256", sha256}};
    if (!artifact_format.empty()) {
        j["artifactFormat"] = artifact_format;
    }
    if (!artifact_name.empty()) {
        j["artifactName"] = artifact_name;
    }
    if (!progress_entity_id.empty()) {
        j["progressEntityId"] = progress_entity_id;
    }
    if (!signature.empty()) {
        j["signature"] = signature;
    }
    if (!signing_key_id.empty()) {
        j["signingKeyId"] = signing_key_id;
    }
    if (size_bytes > 0) {
        j["sizeBytes"] = size_bytes;
    }
    if (apply_after_ms > 0) {
        j["applyAfter"] = apply_after_ms;
    }
    return j;
}

}  // namespace dextro::iot::ota
