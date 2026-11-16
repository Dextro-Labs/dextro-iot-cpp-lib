#include <dextro-iot/procedures/ops/get_status.hpp>

#include <algorithm>

namespace dextro::iot::procedures {

GetStatus::GetStatus(const StatusSource* status) : status_(status)
{
}

std::optional<Reply> GetStatus::validate(const Json& payload) const
{
    if (auto refused = only_keys(payload, {"sections"})) {
        return refused;
    }
    const auto it = payload.find("sections");
    if (it == payload.end()) {
        return std::nullopt;
    }
    if (!it->is_array()) {
        return Reply::invalid("sections não é lista");
    }
    for (std::size_t i = 0; i < it->size(); ++i) {
        const Json& item = (*it)[i];
        if (!item.is_string() || item.get_ref<const std::string&>().size() > 64) {
            return Reply::invalid("sections[" + std::to_string(i) + "] não é texto de até 64");
        }
        // `uniqueItems`.
        if (std::find(it->begin(), it->begin() + static_cast<long>(i), item) !=
            it->begin() + static_cast<long>(i)) {
            return Reply::invalid("sections repetida: " + item.get<std::string>());
        }
    }
    return std::nullopt;
}

Reply GetStatus::execute(const Request& request)
{
    if (status_ == nullptr) {
        return Reply::error(Rescode::ServerError, ErrorCode::Unavailable, "status");
    }

    std::vector<std::string> wanted;
    if (const auto it = request.payload.find("sections"); it != request.payload.end()) {
        wanted = it->get<std::vector<std::string>>();
    }
    if (wanted.empty() || std::find(wanted.begin(), wanted.end(), "all") != wanted.end()) {
        wanted = status_->sections();
    }

    Json out = Json::object();
    for (const std::string& section : wanted) {
        Json block = Json::object();
        if (!status_->fill(section, block)) {
            return Reply::invalid("seção desconhecida: " + section);
        }
        out[section] = std::move(block);
    }
    return Reply::ok(std::move(out));
}

}  // namespace dextro::iot::procedures
