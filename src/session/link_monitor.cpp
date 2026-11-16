#include <dextro-iot/session/link_monitor.hpp>

#include <cerrno>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dextro::iot::session {

LinkMonitor::~LinkMonitor()
{
    close();
}

bool LinkMonitor::open() noexcept
{
    close();
    fd_ = ::socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd_ < 0) {
        return false;
    }
    sockaddr_nl address{};
    address.nl_family = AF_NETLINK;
    address.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_IFADDR |
                        RTMGRP_IPV6_ROUTE;
    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        close();
        return false;
    }
    return true;
}

void LinkMonitor::close() noexcept
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool LinkMonitor::drain() noexcept
{
    if (fd_ < 0) {
        return false;
    }
    bool changed = false;
    alignas(nlmsghdr) char buffer[8192];
    for (;;) {
        const ssize_t n = ::recv(fd_, buffer, sizeof buffer, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            // ENOBUFS: o kernel descartou eventos porque ninguém leu a tempo.
            // Perdeu-se o detalhe, não o fato — houve mudança.
            if (errno == ENOBUFS) {
                changed = true;
                continue;
            }
            break;  // EAGAIN: esvaziou
        }
        if (n == 0) {
            break;
        }
        // Qualquer mensagem destes grupos é mudança de rede. Não precisa
        // decodificar: a sessão reage igual a todas.
        changed = true;
    }
    return changed;
}

}  // namespace dextro::iot::session
