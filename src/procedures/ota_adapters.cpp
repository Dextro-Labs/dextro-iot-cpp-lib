#include <dextro-iot/procedures/ota_adapters.hpp>

#include <dextro-iot/atomic_file.hpp>
#include <dextro-iot/clock.hpp>
#include <dextro-iot/log.hpp>
#include <dextro-iot/ota/event_sink.hpp>

#include <sys/stat.h>

#include <cerrno>

namespace dextro::iot::procedures {
namespace {

bool is_dir(const std::string& path) noexcept
{
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_file(const std::string& path) noexcept
{
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/// `mkdir -p`, sem exceção.
bool make_dirs(const std::string& dir) noexcept
{
    try {
        for (std::size_t i = 1; i <= dir.size(); ++i) {
            if (i == dir.size() || dir[i] == '/') {
                const std::string part = dir.substr(0, i);
                if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) {
                    return false;
                }
            }
        }
    } catch (...) {
        return false;
    }
    return is_dir(dir);
}

}  // namespace

bool is_safe_file_id(std::string_view id) noexcept
{
    if (id.empty() || id.size() > 64) {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

InboxOtaSpool::InboxOtaSpool(std::string inbox_dir, ota::EventSink* events)
    : inbox_dir_(std::move(inbox_dir)), events_(events)
{
}

bool InboxOtaSpool::enqueue(const Json& manifest)
{
    const std::string id = manifest.value("campaignItemId", std::string{});
    if (!is_safe_file_id(id) || !make_dirs(inbox_dir_)) {
        return false;
    }
    const std::string path = inbox_dir_ + "/" + id + ".json";
    if (!write_file_atomic(path, manifest.dump(), 0644)) {
        DEXTRO_IOT_ERR("dextro-iot: não gravou o manifesto do OTA em %s", path.c_str());
        return false;
    }
    const std::string version = manifest.value("version", std::string{});
    DEXTRO_IOT_INFO("dextro-iot: OTA %s entregue ao agente (%s)", version.c_str(), path.c_str());

    if (events_ == nullptr) {
        return true;
    }
    std::int64_t apply_after = 0;
    if (const auto it = manifest.find("applyAfter"); it != manifest.end() && it->is_number_integer()) {
        apply_after = it->get<std::int64_t>();
    }
    const Json args = {{"campaignItemId", id}, {"version", version}, {"applyAfter", apply_after}};
    if (!events_->emit("ota-received", args, clock::wall_ms())) {
        DEXTRO_IOT_WARN("dextro-iot: ota-received de %s não gravado", id.c_str());
    }
    return true;
}

DirectoryKeyring::DirectoryKeyring(std::string dir) : dir_(std::move(dir))
{
}

bool DirectoryKeyring::known(std::string_view key_id) const
{
    if (!is_dir(dir_)) {
        return true;
    }
    // O mesmo alfabeto do `ota::signature::verify_sha256`, menos o `.` no
    // começo (nada de `..`).
    if (key_id.empty() || key_id.size() > 64 || key_id.front() == '.') {
        return false;
    }
    for (const char c : key_id) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return is_file(dir_ + "/" + std::string(key_id) + ".pub");
}

}  // namespace dextro::iot::procedures
