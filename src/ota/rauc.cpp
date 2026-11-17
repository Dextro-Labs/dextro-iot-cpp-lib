#include <dextro-iot/ota/rauc.hpp>

#include <dextro-iot/ota/linux_platform.hpp>

#include <cctype>
#include <cstdlib>
#include <map>
#include <sstream>

namespace dextro::iot::ota {
namespace {

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

RaucInstaller::RaucInstaller(std::string rauc) : rauc_(std::move(rauc))
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
    // A última linha não vazia é onde o rauc diz o motivo.
    std::istringstream in(out);
    std::string line;
    std::string last;
    while (std::getline(in, line)) {
        if (!trim(line).empty()) {
            last = trim(line);
        }
    }
    message = "rauc install saiu com " + std::to_string(rc) + (last.empty() ? "" : ": " + last);
    return false;
}

/* --------------------------------------------------------- RaucHandler --- */

RaucHandler::RaucHandler(Installer& installer) : installer_(installer)
{
}

InstallStep RaucHandler::install(InstallContext& ctx)
{
    Job& job = ctx.job;
    // O `ota-install-started` sai uma vez: um reinício no meio do `rauc
    // install` (o processo caiu, a placa não) instala de novo sem repetir o
    // evento, como no ssat-ota-agent.
    if (!job.handler.value("started", false)) {
        const std::string current = ctx.platform.current_slot();
        const std::string target = current.empty() ? std::string{}
                                                   : ctx.platform.other_slot(current);
        if (current.empty() || target.empty()) {
            return InstallStep::failed(
                "SLOT_UNKNOWN",
                "o boot corrente não é de um slot A/B conhecido (corrente '" + current + "')",
                false);
        }
        job.from_slot = current;
        job.target_slot = target;
        job.handler["started"] = true;
        ctx.save();
        ctx.emit("ota-install-started", {{"slot", target}});
    }
    std::string message;
    if (!installer_.install(ctx.file, message, ctx.keep_going)) {
        return InstallStep::failed("RAUC_INSTALL_FAILED", message, false);
    }
    return InstallStep::reboot();
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
