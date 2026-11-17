// O token `dxd_` e o sd_notify sem libsystemd.
//
// Origem: o teste TokenDoDevice de tests/unit/manifest_test.cpp do
// ssat-ota-agent (origin/devel 76aa940), com os caminhos como parâmetro; os de
// sd_notify são novos (um socket AF_UNIX de verdade no lugar do systemd).
#include <dextro-iot/device_token.hpp>
#include <dextro-iot/systemd_notify.hpp>

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#ifndef DEXTRO_IOT_TEST_DIR
#error "DEXTRO_IOT_TEST_DIR não definido"
#endif

namespace {

std::string tmp(const char* name)
{
    return std::string(DEXTRO_IOT_TEST_DIR) + "/ds-" + name + "-" + std::to_string(::getpid());
}

/// O lado do systemd: um socket de datagrama no caminho de NOTIFY_SOCKET.
class NotifySocket {
public:
    explicit NotifySocket(std::string path) : path_(std::move(path))
    {
        ::unlink(path_.c_str());
        fd_ = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::strncpy(a.sun_path, path_.c_str(), sizeof a.sun_path - 1);
        ok_ = fd_ >= 0 && ::bind(fd_, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0;
        timeval tv{1, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    ~NotifySocket()
    {
        ::close(fd_);
        ::unlink(path_.c_str());
    }
    bool ok() const { return ok_; }
    std::string receive()
    {
        char buf[512];
        const ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
        return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string{};
    }

private:
    std::string path_;
    int fd_ = -1;
    bool ok_ = false;
};

}  // namespace

TEST(TokenDoDevice, ArquivoPrimeiroDepoisOIni)
{
    dextro::iot::DeviceTokenSource src;
    src.file = tmp("token");
    src.ini_path = tmp("ini");
    std::ofstream(src.ini_path) << "; x\n[broker]\ndeviceAuth = errado\n[device]\nserialNumber = S1\n"
                                   "deviceAuth = dxd_do_ini\n";
    EXPECT_EQ(dextro::iot::read_device_token(src), "dxd_do_ini");
    std::ofstream(src.file) << "  dxd_do_arquivo \nlixo\n";
    EXPECT_EQ(dextro::iot::read_device_token(src), "dxd_do_arquivo");

    // Seção e chave são do produto.
    dextro::iot::DeviceTokenSource outro = src;
    outro.file.clear();
    outro.ini_section = "broker";
    EXPECT_EQ(dextro::iot::read_device_token(outro), "errado");

    std::remove(src.file.c_str());
    std::remove(src.ini_path.c_str());
    EXPECT_EQ(dextro::iot::read_device_token(src), "");
    EXPECT_EQ(dextro::iot::read_device_token({}), "");
}

TEST(SdNotify, ForaDoSystemdNaoFazNada)
{
    ::unsetenv("NOTIFY_SOCKET");
    ::unsetenv("WATCHDOG_USEC");
    EXPECT_FALSE(dextro::iot::systemd::notify_ready("pronto"));
    EXPECT_EQ(dextro::iot::systemd::watchdog_usec(), 0U);
    dextro::iot::systemd::Watchdog dog;
    EXPECT_FALSE(dog.enabled());
    EXPECT_EQ(dog.tick(1000), -1);
}

TEST(SdNotify, ReadyStatusEWatchdogChegamAoSocket)
{
    const std::string path = tmp("notify");
    NotifySocket systemd(path);
    ASSERT_TRUE(systemd.ok());
    ::setenv("NOTIFY_SOCKET", path.c_str(), 1);
    ::setenv("WATCHDOG_USEC", "20000000", 1);  // 20 s
    ::setenv("WATCHDOG_PID", std::to_string(::getpid()).c_str(), 1);

    EXPECT_TRUE(dextro::iot::systemd::notify_ready("conectado"));
    EXPECT_EQ(systemd.receive(), "READY=1\nSTATUS=conectado");
    EXPECT_TRUE(dextro::iot::systemd::notify_status("baixando"));
    EXPECT_EQ(systemd.receive(), "STATUS=baixando");

    EXPECT_EQ(dextro::iot::systemd::watchdog_usec(), 20000000U);
    dextro::iot::systemd::Watchdog dog;
    ASSERT_TRUE(dog.enabled());
    EXPECT_EQ(dog.tick(1000), 10000) << "na metade do período";
    EXPECT_EQ(systemd.receive(), "WATCHDOG=1");
    EXPECT_EQ(dog.tick(6000), 5000) << "antes da hora não manda";
    EXPECT_EQ(dog.tick(11000), 10000);
    EXPECT_EQ(systemd.receive(), "WATCHDOG=1");

    // O watchdog de outro processo (o daemon forkou) não é deste.
    ::setenv("WATCHDOG_PID", "1", 1);
    EXPECT_EQ(dextro::iot::systemd::watchdog_usec(), 0U);

    ::unsetenv("NOTIFY_SOCKET");
    ::unsetenv("WATCHDOG_USEC");
    ::unsetenv("WATCHDOG_PID");
}
