#include <dextro-iot/ota/rauc.hpp>

#include <dextro-iot/ota/linux_platform.hpp>

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <sstream>

namespace dextro::iot::ota {
namespace {

using Json = nlohmann::json;

/// Linhas do fim da saída do `rauc install` que vão na mensagem.
constexpr std::size_t kMessageLines = 3;
/// O daemon ocupado: pergunta de novo a cada tanto.
constexpr std::int64_t kBusyPollMs = 5000;
/// `rauc install` por item: o primeiro, e um segundo quando o primeiro não
/// escreveu o slot e o daemon não disse por quê.
constexpr int kMaxRuns = 2;
/// O `installed.timestamp` do RAUC é em segundos (RAUC_FORMAT_ISO_8601).
constexpr std::int64_t kTimestampSlackMs = 1000;

std::string trim(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

bool same_name(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

/* --------------------------------------------------------- rauc install --- */

RaucInstaller::RaucInstaller(std::string rauc, std::string busctl)
    : rauc_(std::move(rauc)), busctl_(std::move(busctl))
{
}

bool RaucInstaller::install(const std::string& bundle, std::string& message,
                            const KeepGoing& keep_going)
{
    std::string out;
    const int rc = run_command({rauc_, "install", bundle}, out, keep_going);
    if (rc == 0) {
        return true;
    }
    // As últimas linhas: o "D-Bus error while installing" do cliente vem
    // DEPOIS do motivo ("Failed to contact rauc service: ...", "RAUC service
    // disappeared"), e sozinho não diz nada (src/main.c, install_start).
    std::istringstream in(out);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(in, line)) {
        if (!trim(line).empty()) {
            lines.push_back(trim(line));
        }
    }
    std::string tail;
    const std::size_t from = lines.size() > kMessageLines ? lines.size() - kMessageLines : 0;
    for (std::size_t i = from; i < lines.size(); ++i) {
        tail += (tail.empty() ? "" : " | ") + lines[i];
    }
    message = "rauc install saiu com " + std::to_string(rc) + (tail.empty() ? "" : ": " + tail);
    return false;
}

RaucDaemon RaucInstaller::daemon()
{
    std::string out;
    if (run_command({busctl_, "--system", "get-property", "de.pengutronix.rauc", "/",
                     "de.pengutronix.rauc.Installer", "Operation", "LastError"},
                    out) != 0) {
        return {};
    }
    return parse_rauc_daemon(out);
}

std::optional<RaucSlotRecord> RaucInstaller::slot(const std::string& bootname)
{
    std::string out;
    if (run_command({rauc_, "status", "--detailed", "--output-format=json"}, out) != 0) {
        return std::nullopt;
    }
    return parse_rauc_slot(out, bootname);
}

/* -------------------------------------------------- D-Bus e JSON do RAUC --- */

namespace {

/// `s "valor"` → `valor`, com os escapes de C que o busctl usa nas strings.
std::optional<std::string> busctl_string(const std::string& line)
{
    const std::string t = trim(line);
    if (t.size() < 4 || t.compare(0, 3, "s \"") != 0 || t.back() != '"') {
        return std::nullopt;
    }
    std::string out;
    for (std::size_t i = 3; i + 1 < t.size(); ++i) {
        if (t[i] == '\\' && i + 2 < t.size()) {
            const char next = t[++i];
            out.push_back(next == 'n' || next == 't' ? ' ' : next);
            continue;
        }
        out.push_back(t[i]);
    }
    return out;
}

}  // namespace

RaucDaemon parse_rauc_daemon(const std::string& busctl_output)
{
    std::istringstream in(busctl_output);
    std::string line;
    std::vector<std::string> values;
    while (std::getline(in, line)) {
        if (auto v = busctl_string(line)) {
            values.push_back(std::move(*v));
        }
    }
    RaucDaemon d;
    if (values.empty()) {
        return d;
    }
    d.operation = values[0] == "idle" ? RaucDaemon::Operation::Idle : RaucDaemon::Operation::Busy;
    if (values.size() > 1) {
        d.last_error = values[1];
    }
    return d;
}

namespace {

/// `2026-11-26T10:04:10Z` (RAUC_FORMAT_ISO_8601, UTC) → epoch ms; -1 se não é.
std::int64_t iso_utc_ms(const std::string& text)
{
    std::tm tm{};
    char z = 0;
    if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%c", &tm.tm_year, &tm.tm_mon,
                    &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &z) != 7 ||
        z != 'Z') {
        return -1;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    const std::time_t t = ::timegm(&tm);  // timegm(3): UTC, sem o fuso do processo
    return t < 0 ? -1 : static_cast<std::int64_t>(t) * 1000;
}

}  // namespace

std::optional<RaucSlotRecord> parse_rauc_slot(const std::string& json,
                                              const std::string& bootname)
{
    const Json j = Json::parse(json, nullptr, false);
    if (!j.is_object() || !j.contains("slots") || !j["slots"].is_array()) {
        return std::nullopt;
    }
    const std::string primary = j.value("boot_primary", std::string{});
    for (const Json& entry : j["slots"]) {
        if (!entry.is_object()) {
            continue;
        }
        for (const auto& [name, slot] : entry.items()) {
            if (!slot.is_object() || !same_name(slot.value("bootname", std::string{}), bootname)) {
                continue;
            }
            const Json st = slot.value("slot_status", Json::object());
            if (!st.is_object() || st.empty()) {
                return std::nullopt;
            }
            RaucSlotRecord r;
            r.status = st.value("status", std::string{});
            r.build = st.value("bundle", Json::object()).value("build", std::string{});
            r.installed_ms =
                iso_utc_ms(st.value("installed", Json::object()).value("timestamp", std::string{}));
            r.primary = !primary.empty() && primary == name;
            return r;
        }
    }
    return std::nullopt;
}

/* --------------------------------------------------------- RaucHandler --- */

RaucHandler::RaucHandler(Installer& installer) : installer_(installer)
{
}

std::optional<InstallStep> RaucHandler::begin(InstallContext& ctx)
{
    Job& job = ctx.job;
    // O `ota-install-started` sai uma vez: um reinício no meio do `rauc
    // install` (o processo caiu, a placa não) retoma sem repetir o evento.
    if (job.handler.value("started", false)) {
        return std::nullopt;
    }
    const std::string current = ctx.platform.current_slot();
    const std::string target = current.empty() ? std::string{} : ctx.platform.other_slot(current);
    if (current.empty() || target.empty()) {
        return InstallStep::failed(
            "SLOT_UNKNOWN",
            "o boot corrente não é de um slot A/B conhecido (corrente '" + current + "')", false);
    }
    job.from_slot = current;
    job.target_slot = target;
    job.handler["started"] = true;
    // A régua do `installed.timestamp`: o slot só vale se foi escrito depois.
    job.handler["startedAtMs"] = ctx.platform.wall_ms();
    ctx.save();
    ctx.emit("ota-install-started", {{"slot", target}});
    return std::nullopt;
}

bool RaucHandler::written(const Job& job)
{
    // Estado de um agente antigo, sem a régua: não dá para dizer que o
    // registro do slot é desta instalação.
    const std::int64_t since = job.handler.value("startedAtMs", static_cast<std::int64_t>(0));
    if (since <= 0) {
        return false;
    }
    const auto rec = installer_.slot(job.target_slot);
    // O timestamp do RAUC tem resolução de segundo: a margem cobre o corte.
    return rec && rec->status == "ok" && rec->primary && rec->installed_ms >= 0 &&
           rec->installed_ms + kTimestampSlackMs >= since;
}

std::optional<InstallStep> RaucHandler::judge(const Job& job, const RaucDaemon& daemon)
{
    if (written(job)) {
        return InstallStep::reboot({{"confirmedBy", "rauc-status"}});
    }
    const std::string cli = job.handler.value("cliMessage", std::string{});
    const std::string before = job.handler.value("lastErrorBefore", std::string{});
    if (!daemon.last_error.empty() && daemon.last_error != before) {
        return InstallStep::failed("RAUC_INSTALL_FAILED", daemon.last_error, false,
                                   {{"client", cli}});
    }
    if (job.handler.value("runs", 0) >= kMaxRuns) {
        return InstallStep::failed(
            "RAUC_INSTALL_FAILED",
            (cli.empty() ? std::string("o rauc install terminou") : cli) +
                " (o slot " + job.target_slot + " não foi escrito e o rauc.service não deu motivo)",
            true);
    }
    return std::nullopt;
}

InstallStep RaucHandler::install(InstallContext& ctx)
{
    Job& job = ctx.job;
    if (auto refused = begin(ctx)) {
        return *refused;
    }
    // No máximo kMaxRuns voltas: cada uma ou decide, ou roda o rauc install.
    for (;;) {
        const RaucDaemon daemon = installer_.daemon();
        if (daemon.operation == RaucDaemon::Operation::Busy) {
            return InstallStep::pending(kBusyPollMs);
        }
        if (job.handler.value("runs", 0) > 0) {
            if (auto verdict = judge(job, daemon)) {
                return *verdict;
            }
        }
        job.handler["runs"] = job.handler.value("runs", 0) + 1;
        job.handler["lastErrorBefore"] = daemon.last_error;
        job.handler.erase("cliMessage");
        ctx.save();  // antes: se o agente cair no meio, a volta seguinte sabe que rodou
        std::string message;
        if (installer_.install(ctx.file, message, ctx.keep_going)) {
            return InstallStep::reboot();
        }
        job.handler["cliMessage"] = message;
        ctx.save();
    }
}

/* ---------------------------------------------------------- rauc status --- */

std::string shell_unquote(const std::string& text)
{
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '\'') {
            const auto end = text.find('\'', i + 1);
            if (end == std::string::npos) {
                out.append(text, i + 1, std::string::npos);
                break;
            }
            out.append(text, i + 1, end - i - 1);
            i = end + 1;
        } else if (c == '"') {
            ++i;
            while (i < text.size() && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < text.size() &&
                    (text[i + 1] == '"' || text[i + 1] == '\\' || text[i + 1] == '$' ||
                     text[i + 1] == '`')) {
                    ++i;
                }
                out.push_back(text[i]);
                ++i;
            }
            ++i;
        } else if (c == '\\' && i + 1 < text.size()) {
            out.push_back(text[i + 1]);
            i += 2;
        } else {
            out.push_back(c);
            ++i;
        }
    }
    return out;
}

bool parse_rauc_status(const std::string& text, RaucStatus& out)
{
    std::map<std::string, std::string> vars;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            continue;
        }
        vars[line.substr(0, eq)] = shell_unquote(trim(line.substr(eq + 1)));
    }
    const auto slots = vars.find("RAUC_SYSTEM_SLOTS");
    if (slots == vars.end()) {
        return false;
    }
    out = RaucStatus{};
    out.compatible = vars["RAUC_SYSTEM_COMPATIBLE"];
    out.booted = vars["RAUC_SYSTEM_BOOTED_BOOTNAME"];
    out.primary = vars["RAUC_BOOT_PRIMARY"];

    // RAUC_SYSTEM_SLOTS e os índices 1..n saem na mesma ordem (o mesmo
    // percurso da tabela de slots, r_status_formatter_shell).
    std::istringstream names(slots->second);
    std::string name;
    int n = 0;
    while (names >> name) {
        ++n;
        const std::string i = std::to_string(n);
        RaucSlot s;
        s.name = name;
        s.cls = vars["RAUC_SLOT_CLASS_" + i];
        s.state = vars["RAUC_SLOT_STATE_" + i];
        s.bootname = vars["RAUC_SLOT_BOOTNAME_" + i];
        s.boot_status = vars["RAUC_SLOT_BOOT_STATUS_" + i];
        out.slots.push_back(std::move(s));
    }
    return true;
}

const RaucSlot* RaucStatus::by_bootname(const std::string& bootname) const noexcept
{
    if (bootname.empty()) {
        return nullptr;
    }
    for (const RaucSlot& s : slots) {
        if (same_name(s.bootname, bootname)) {
            return &s;
        }
    }
    return nullptr;
}

std::string RaucStatus::other_bootname(const std::string& bootname) const
{
    std::string other;
    for (const RaucSlot& s : slots) {
        if (s.bootname.empty() || same_name(s.bootname, bootname) || same_name(s.bootname, other)) {
            continue;
        }
        if (!other.empty()) {
            return {};  // mais de um candidato: não é um par A/B
        }
        other = s.bootname;
    }
    return other;
}

}  // namespace dextro::iot::ota
