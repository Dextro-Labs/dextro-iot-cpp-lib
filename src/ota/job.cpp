#include <dextro-iot/ota/job.hpp>

#include <utility>

namespace dextro::iot::ota {
namespace {

constexpr std::pair<Stage, std::string_view> kStages[] = {
    {Stage::Received, "received"},     {Stage::Downloading, "downloading"},
    {Stage::Downloaded, "downloaded"}, {Stage::Verified, "verified"},
    {Stage::Installing, "installing"}, {Stage::Installed, "installed"},
    {Stage::Rebooting, "rebooting"},   {Stage::Confirming, "confirming"},
    {Stage::Done, "done"},
};

}  // namespace

std::string_view stage_name(Stage stage) noexcept
{
    for (const auto& [s, name] : kStages) {
        if (s == stage) {
            return name;
        }
    }
    return "?";
}

std::optional<Stage> stage_from_name(std::string_view name) noexcept
{
    for (const auto& [s, n] : kStages) {
        if (n == name) {
            return s;
        }
    }
    return std::nullopt;
}

nlohmann::json Job::to_json() const
{
    return {{"manifest", manifest.to_json()},
            {"stage", std::string(stage_name(stage))},
            {"attempt", attempt},
            {"nextTryMs", next_try_ms},
            {"downloadMs", download_ms},
            {"bytes", bytes},
            {"fromSlot", from_slot},
            {"targetSlot", target_slot},
            {"installBootId", install_boot_id},
            {"rebootTries", reboot_tries},
            {"rebootFailureReported", reboot_failure_reported},
            {"result", result},
            {"handler", handler.is_object() ? handler : nlohmann::json::object()}};
}

std::optional<Job> Job::from_json(const nlohmann::json& j)
{
    if (!j.is_object() || !j.contains("manifest") || !j["manifest"].is_object()) {
        return std::nullopt;
    }
    std::string why;
    auto m = Manifest::parse(j["manifest"].dump(), why);
    if (!m) {
        return std::nullopt;
    }
    try {
        const auto stage = stage_from_name(j.value("stage", std::string{}));
        if (!stage) {
            return std::nullopt;
        }
        Job job;
        job.manifest = std::move(*m);
        job.stage = *stage;
        job.attempt = j.value("attempt", 0);
        job.next_try_ms = j.value("nextTryMs", std::int64_t{0});
        job.download_ms = j.value("downloadMs", std::int64_t{0});
        job.bytes = j.value("bytes", std::int64_t{0});
        job.from_slot = j.value("fromSlot", std::string{});
        job.target_slot = j.value("targetSlot", std::string{});
        job.install_boot_id = j.value("installBootId", std::string{});
        job.reboot_tries = j.value("rebootTries", 0);
        job.reboot_failure_reported = j.value("rebootFailureReported", false);
        job.result = j.value("result", std::string{});
        if (const auto it = j.find("handler"); it != j.end() && it->is_object()) {
            job.handler = *it;
        }
        return job;
    } catch (const nlohmann::json::exception&) {
        // Um campo com o tipo errado (arquivo de outra versão, editado à mão).
        return std::nullopt;
    }
}

}  // namespace dextro::iot::ota
