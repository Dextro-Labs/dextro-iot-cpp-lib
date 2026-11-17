#include <dextro-iot/ota/fs.hpp>

#include <dextro-iot/atomic_file.hpp>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>

namespace dextro::iot::ota::fs {
namespace {

bool ends_with(std::string_view s, std::string_view suffix) noexcept
{
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

}  // namespace

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
    struct stat st {};
    return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool write_atomic(const std::string& path, std::string_view content) noexcept
{
    return write_file_atomic(path, content, 0644);
}

bool read_file(const std::string& path, std::string& out, std::size_t max_bytes) noexcept
{
    try {
        std::FILE* f = std::fopen(path.c_str(), "rbe");
        if (f == nullptr) {
            return false;
        }
        out.clear();
        char buf[4096];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
            if (out.size() + n > max_bytes) {
                std::fclose(f);
                return false;
            }
            out.append(buf, n);
        }
        const bool ok = std::ferror(f) == 0;
        std::fclose(f);
        return ok;
    } catch (...) {
        return false;
    }
}

std::int64_t file_size(const std::string& path) noexcept
{
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        return -1;
    }
    return static_cast<std::int64_t>(st.st_size);
}

bool exists(const std::string& path) noexcept
{
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

bool remove_file(const std::string& path) noexcept
{
    return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

std::vector<std::string> list(const std::string& dir, std::string_view suffix)
{
    std::vector<std::string> names;
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) {
        return names;
    }
    while (const dirent* e = ::readdir(d)) {
        const std::string_view name = e->d_name;
        if (name.empty() || name.front() == '.' || !ends_with(name, suffix)) {
            continue;
        }
        names.emplace_back(name);
    }
    ::closedir(d);
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace dextro::iot::ota::fs
