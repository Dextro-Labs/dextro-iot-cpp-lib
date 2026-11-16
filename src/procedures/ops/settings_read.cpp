#include <dextro-iot/procedures/ops/settings_read.hpp>

namespace dextro::iot::procedures {

SettingsRead::SettingsRead(const SettingsStore* settings) : settings_(settings)
{
}

std::optional<Reply> SettingsRead::validate(const Json& payload) const
{
    if (auto refused = only_keys(payload, {"refs", "from", "count"})) {
        return refused;
    }
    // Paginação (opcional): `from` é a posição do primeiro item na coleção
    // (0 = o primeiro), `count` quantos. Sem os dois, a coleção inteira, como
    // sempre foi.
    if (payload.contains("from")) {
        if (auto refused = integer_field(payload, "from", 0)) {
            return refused;
        }
    }
    if (payload.contains("count")) {
        if (auto refused = integer_field(payload, "count", 1)) {
            return refused;
        }
        if (payload["count"].get<std::int64_t>() > static_cast<std::int64_t>(kMaxPage)) {
            return Reply::invalid("count acima de " + std::to_string(kMaxPage));
        }
    }
    const auto it = payload.find("refs");
    if (it == payload.end()) {
        return std::nullopt;
    }
    if (!it->is_array()) {
        return Reply::invalid("refs não é lista");
    }
    for (const Json& ref : *it) {
        // `SettingRef`: texto de 1 a 64.
        if (!ref.is_string() || ref.get_ref<const std::string&>().empty() ||
            ref.get_ref<const std::string&>().size() > 64) {
            return Reply::invalid("refs tem item que não é texto de 1 a 64");
        }
    }
    return std::nullopt;
}

Reply SettingsRead::execute(const Request& request)
{
    if (settings_ == nullptr) {
        return Reply::error(Rescode::ServerError, ErrorCode::Unavailable, "settings");
    }
    std::vector<std::string> refs;
    if (const auto it = request.payload.find("refs"); it != request.payload.end()) {
        refs = it->get<std::vector<std::string>>();
    }
    Json all = settings_->read(refs);
    const bool paged = request.payload.contains("from") || request.payload.contains("count");
    if (!paged) {
        return Reply::ok({{"version", settings_->version()}, {"settings", std::move(all)}});
    }
    const std::size_t from =
        static_cast<std::size_t>(request.payload.value("from", static_cast<std::int64_t>(0)));
    const std::size_t count = static_cast<std::size_t>(
        request.payload.value("count", static_cast<std::int64_t>(kMaxPage)));
    Json page = Json::array();
    for (std::size_t i = from; i < all.size() && page.size() < count; ++i) {
        page.push_back(std::move(all[i]));
    }
    const bool more = from + page.size() < all.size();
    return Reply::ok({{"version", settings_->version()},
                      {"settings", std::move(page)},
                      {"more", more}});
}

}  // namespace dextro::iot::procedures
