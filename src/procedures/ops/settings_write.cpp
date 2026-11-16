#include <dextro-iot/procedures/ops/settings_write.hpp>

#include <algorithm>

namespace dextro::iot::procedures {

SettingsWrite::SettingsWrite(SettingsStore* settings, SessionControl* session)
    : settings_(settings), session_(session)
{
}

std::optional<Reply> SettingsWrite::validate(const Json& payload) const
{
    if (auto refused = only_keys(payload, {"version", "settings"})) {
        return refused;
    }
    if (auto refused = require(payload, "version")) {
        return refused;
    }
    if (auto refused = require(payload, "settings")) {
        return refused;
    }
    if (auto refused = integer_field(payload, "version", 1)) {
        return refused;
    }

    const Json& settings = payload["settings"];
    if (!settings.is_array() || settings.size() > kMaxItems) {
        return Reply::invalid("settings não é lista de até 500");
    }
    std::vector<std::string> seen;
    seen.reserve(settings.size() * 2);
    for (std::size_t i = 0; i < settings.size(); ++i) {
        const Json& item = settings[i];
        const std::string where = "settings[" + std::to_string(i) + "]";
        if (!item.is_object()) {
            return Reply::invalid(where + " não é objeto");
        }
        // Os obrigatórios de `SettingItem`. O resto da forma (enumOptions,
        // limites) é conferido item a item pelo store, que é quem sabe o que
        // cada setting aceita — e o que não passa vai para `rejected`, não 400.
        for (const char* key : {"index", "name", "type", "value", "default"}) {
            if (!item.contains(key)) {
                return Reply::invalid(where + " sem " + key);
            }
        }
        if (!item["name"].is_string() || !item["index"].is_string()) {
            return Reply::invalid(where + " com name/index que não é texto");
        }
        // `index` e `name` únicos na coleção (A.1.1) — duas linhas com o mesmo
        // nome e valores diferentes não têm resposta certa.
        for (const char* key : {"index", "name"}) {
            std::string tag = std::string(key) + ":" + item[key].get<std::string>();
            if (std::find(seen.begin(), seen.end(), tag) != seen.end()) {
                return Reply::error(Rescode::BadRequest, ErrorCode::InvalidSetting,
                                    where + " repete " + key);
            }
            seen.push_back(std::move(tag));
        }
    }
    return std::nullopt;
}

Reply SettingsWrite::execute(const Request& request)
{
    if (settings_ == nullptr) {
        return Reply::error(Rescode::ServerError, ErrorCode::Unavailable, "settings");
    }

    const auto version = request.payload["version"].get<std::int64_t>();
    const std::int64_t applied = settings_->version();
    if (version < applied) {
        return Reply::error(Rescode::Conflict, ErrorCode::StaleVersion,
                            "version " + std::to_string(version) + " < aplicada " +
                                std::to_string(applied));
    }

    std::vector<Rejection> rejected;
    bool session_affected = false;
    if (!settings_->apply(version, request.payload["settings"], rejected, session_affected)) {
        return Reply::error(Rescode::ServerError, ErrorCode::Storage, "settings");
    }

    Json out = Json::array();
    for (const Rejection& item : rejected) {
        out.push_back({{"ref", item.ref}, {"reason", item.reason.substr(0, 512)}});
    }

    // Broker mudou: a sessão atual está falando com o destino antigo. Quem
    // reabre é a thread, depois de publicar esta resposta.
    if (session_affected && session_ != nullptr) {
        session_->request_reconnect();
    }
    return Reply::ok({{"appliedVersion", version}, {"rejected", std::move(out)}});
}

}  // namespace dextro::iot::procedures
