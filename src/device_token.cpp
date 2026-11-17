#include <dextro-iot/device_token.hpp>

#include <dextro-iot/atomic_file.hpp>

#include <sstream>

namespace dextro::iot {
namespace {

/// O teto do que se lê: um token é curto, um `.ini` de settings não passa
/// disto. Arquivo maior não é o que se espera ali.
constexpr std::size_t kMaxTokenFile = 4096;
constexpr std::size_t kMaxIniFile = 256 * 1024;

std::string trim(const std::string& s)
{
    const char* ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) {
        return {};
    }
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

std::string from_ini(const std::string& text, const std::string& section_name,
                     const std::string& key)
{
    std::istringstream in(text);
    std::string line;
    std::string section;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        const auto eq = line.find('=');
        if (section != section_name || eq == std::string::npos) {
            continue;
        }
        if (trim(line.substr(0, eq)) == key) {
            return trim(line.substr(eq + 1));
        }
    }
    return {};
}

}  // namespace

std::string read_device_token(const DeviceTokenSource& source)
{
    std::string text;
    if (!source.file.empty() && read_file(source.file, text) && text.size() <= kMaxTokenFile) {
        std::istringstream in(text);
        std::string first;
        std::getline(in, first);
        first = trim(first);
        if (!first.empty()) {
            return first;
        }
    }
    if (!source.ini_path.empty() && read_file(source.ini_path, text) && text.size() <= kMaxIniFile) {
        return from_ini(text, source.ini_section, source.ini_key);
    }
    return {};
}

}  // namespace dextro::iot
