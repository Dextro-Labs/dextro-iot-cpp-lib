#include <dextro-iot/systemd_notify.hpp>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace dextro::iot::systemd {
namespace {

/// O endereço a partir de NOTIFY_SOCKET. Um caminho começando por '@' é do
/// espaço abstrato: o '@' vira NUL e o nome não existe no sistema de
/// arquivos. Esquecer esse caso faz o notify falhar em silêncio exatamente nas
/// configurações em que o systemd usa socket abstrato.
bool address_of(const char* path, sockaddr_un& address, socklen_t& length) noexcept
{
    const std::size_t size = std::strlen(path);
    if (size == 0 || size >= sizeof address.sun_path) {
        return false;
    }
    if (path[0] != '/' && path[0] != '@') {
        return false;
    }
    std::memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path, size);
    if (address.sun_path[0] == '@') {
        address.sun_path[0] = '\0';
    }
    length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + size);
    return true;
}

/// `head` + `status` numa mensagem só; grande demais, só o `fallback`.
bool notify_with_status(std::string_view head, std::string_view status,
                        std::string_view fallback) noexcept
{
    char message[256];
    if (head.size() + status.size() > sizeof message) {
        return fallback.empty() ? false : notify(fallback);
    }
    std::memcpy(message, head.data(), head.size());
    std::memcpy(message + head.size(), status.data(), status.size());
    return notify(std::string_view(message, head.size() + status.size()));
}

}  // namespace

bool notify(std::string_view state) noexcept
{
    const char* path = std::getenv("NOTIFY_SOCKET");
    if (path == nullptr || state.empty()) {
        return false;
    }
    sockaddr_un address{};
    socklen_t length = 0;
    if (!address_of(path, address, length)) {
        return false;
    }
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    const ssize_t sent = ::sendto(fd, state.data(), state.size(), MSG_NOSIGNAL,
                                  reinterpret_cast<const sockaddr*>(&address), length);
    ::close(fd);
    return sent == static_cast<ssize_t>(state.size());
}

bool notify_ready(std::string_view status) noexcept
{
    return notify_with_status("READY=1\nSTATUS=", status, "READY=1");
}

bool notify_status(std::string_view status) noexcept
{
    return notify_with_status("STATUS=", status, {});
}

bool notify_stopping(std::string_view status) noexcept
{
    return notify_with_status("STOPPING=1\nSTATUS=", status, "STOPPING=1");
}

bool notify_watchdog() noexcept
{
    return notify("WATCHDOG=1");
}

std::uint64_t watchdog_usec() noexcept
{
    const char* value = std::getenv("WATCHDOG_USEC");
    if (value == nullptr) {
        return 0;
    }
    // WATCHDOG_PID existe para o caso de o daemon ter forkado: o watchdog é do
    // processo nomeado, e mais ninguém deve alimentá-lo.
    if (const char* owner = std::getenv("WATCHDOG_PID"); owner != nullptr) {
        const long pid = std::strtol(owner, nullptr, 10);
        if (pid != static_cast<long>(::getpid())) {
            return 0;
        }
    }
    return std::strtoull(value, nullptr, 10);
}

Watchdog::Watchdog() noexcept
{
    const std::uint64_t usec = watchdog_usec();
    period_ms_ = static_cast<std::int64_t>(usec / 2000U);
    if (usec > 0 && period_ms_ == 0) {
        period_ms_ = 1;
    }
}

std::int64_t Watchdog::tick(std::int64_t now_ms) noexcept
{
    if (period_ms_ <= 0) {
        return -1;
    }
    if (next_ms_ < 0 || now_ms >= next_ms_) {
        (void)notify_watchdog();
        next_ms_ = now_ms + period_ms_;
    }
    return next_ms_ - now_ms;
}

}  // namespace dextro::iot::systemd
