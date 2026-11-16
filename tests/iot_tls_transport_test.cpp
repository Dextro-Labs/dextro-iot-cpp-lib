// O TlsTransport contra um servidor TLS de verdade, no próprio processo.
//
// Toca o kernel (sockets em 127.0.0.1) e o OpenSSL inteiro, mas roda em
// qualquer CI Linux em poucos segundos — como o teste da serial com ptys. É o
// que prova o caminho que a sessão usa na placa: connect não bloqueante,
// handshake em passos, nome do servidor conferido, mTLS, e cada falha com o
// nome certo.

#include <dextro-iot/session/tls_transport.hpp>

#include <gtest/gtest.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <pthread.h>
#include <cstdio>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#ifndef DEXTRO_IOT_TEST_DIR
#error "DEXTRO_IOT_TEST_DIR não definido — ver tests/CMakeLists.txt"
#endif

using dextro::iot::session::Endpoint;
using dextro::iot::session::Failure;
using dextro::iot::session::TlsTransport;
using Step = dextro::iot::session::Transport::Step;

namespace {

/// Um diretório por PROCESSO: o ctest roda cada teste num processo próprio,
/// em paralelo, e cada um gera a sua CA. No mesmo diretório, uma CA
/// sobrescreveria a outra no meio do handshake alheio.
std::string dir()
{
    static const std::string path = [] {
        std::string p = std::string(DEXTRO_IOT_TEST_DIR) + "/iot-tls-" + std::to_string(::getpid());
        ::mkdir(p.c_str(), 0700);
        return p;
    }();
    return path;
}

void write_pem(const std::string& path, EVP_PKEY* key, X509* cert)
{
    FILE* f = std::fopen(path.c_str(), "w");
    ASSERT_NE(f, nullptr);
    if (key != nullptr) {
        PEM_write_PrivateKey(f, key, nullptr, nullptr, 0, nullptr, nullptr);
    }
    if (cert != nullptr) {
        PEM_write_X509(f, cert);
    }
    std::fclose(f);
}

/// Uma CA de bancada que emite certificados de servidor e de cliente.
class Ca {
public:
    explicit Ca(const char* name) : key_(EVP_EC_gen("P-256"))
    {
        cert_ = issue(name, nullptr, key_, /*ca=*/true, nullptr);
    }
    ~Ca()
    {
        X509_free(cert_);
        EVP_PKEY_free(key_);
    }
    Ca(const Ca&) = delete;
    Ca& operator=(const Ca&) = delete;

    /// Um certificado para `key`, com SAN (`nullptr` = sem SAN).
    X509* issue(const char* cn, const char* san, EVP_PKEY* key, bool ca, X509* issuer_cert)
    {
        X509* cert = X509_new();
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), ++serial_);
        X509_gmtime_adj(X509_getm_notBefore(cert), -60);
        X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
        X509_NAME_add_entry_by_txt(X509_get_subject_name(cert), "CN", MBSTRING_UTF8,
                                   reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
        X509_set_issuer_name(cert, X509_get_subject_name(issuer_cert != nullptr ? issuer_cert : cert));
        X509_set_pubkey(cert, key);

        X509V3_CTX ctx;
        X509V3_set_ctx_nodb(&ctx);
        X509V3_set_ctx(&ctx, issuer_cert != nullptr ? issuer_cert : cert, cert, nullptr, nullptr, 0);
        auto add = [&](int nid, const char* value) {
            X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
            X509_add_ext(cert, ext, -1);
            X509_EXTENSION_free(ext);
        };
        add(NID_basic_constraints, ca ? "critical,CA:TRUE" : "CA:FALSE");
        if (ca) {
            add(NID_key_usage, "critical,keyCertSign,cRLSign");
        }
        if (san != nullptr) {
            add(NID_subject_alt_name, san);
        }
        X509_sign(cert, ca && issuer_cert == nullptr ? key : key_, EVP_sha256());
        return cert;
    }

    /// Gera um par, emite, e grava `<name>.pem` (cert) e `<name>.key`.
    void emit(const std::string& name, const char* cn, const char* san)
    {
        EVP_PKEY* key = EVP_EC_gen("P-256");
        X509* cert = issue(cn, san, key, false, cert_);
        write_pem(dir() + "/" + name + ".pem", nullptr, cert);
        write_pem(dir() + "/" + name + ".key", key, nullptr);
        X509_free(cert);
        EVP_PKEY_free(key);
    }

    void save(const std::string& name) const { write_pem(dir() + "/" + name + ".pem", nullptr, cert_); }

private:
    EVP_PKEY* key_ = nullptr;
    X509* cert_ = nullptr;
    long serial_ = 0;
};

/// O broker de mentira, só a camada TLS. Uma thread, uma conexão por vez.
class Server {
public:
    enum class Mode {
        Echo,              ///< devolve o que chegar
        CloseAfterHello,   ///< termina o handshake e fecha (FIN)
    };

    Server(Mode mode, const std::string& client_ca, bool tls12_only) : mode_(mode)
    {
        ctx_ = SSL_CTX_new(TLS_server_method());
        SSL_CTX_use_certificate_file(ctx_, (dir() + "/tls-server.pem").c_str(), SSL_FILETYPE_PEM);
        SSL_CTX_use_PrivateKey_file(ctx_, (dir() + "/tls-server.key").c_str(), SSL_FILETYPE_PEM);
        SSL_CTX_load_verify_locations(ctx_, client_ca.c_str(), nullptr);
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
        if (tls12_only) {
            // No TLS 1.2 a recusa do certificado do cliente sai DENTRO do
            // handshake; no 1.3, no primeiro read depois dele. Os dois
            // caminhos têm teste.
            SSL_CTX_set_max_proto_version(ctx_, TLS1_2_VERSION);
        }

        listen_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        const int on = 1;
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        ::listen(listen_, 4);
        socklen_t size = sizeof a;
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&a), &size);
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~Server()
    {
        stop_ = true;
        ::shutdown(listen_, SHUT_RDWR);
        ::close(listen_);
        thread_.join();
        SSL_CTX_free(ctx_);
    }

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    std::uint16_t port() const { return port_; }

private:
    void serve()
    {
        // O SIGPIPE bloqueado SÓ nesta thread: o servidor de bancada usa o BIO
        // padrão do OpenSSL, que escreve com write(), e no TLS 1.3 ele manda
        // os session tickets depois do handshake — num socket que o cliente
        // pode já ter fechado. A thread do teste, onde roda o TlsTransport,
        // continua com o SIGPIPE padrão, que é o que dá sentido ao teste de
        // escrita depois do fechamento.
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &mask, nullptr);

        while (!stop_) {
            const int fd = ::accept(listen_, nullptr, nullptr);
            if (fd < 0) {
                return;
            }
            SSL* ssl = SSL_new(ctx_);
            SSL_set_fd(ssl, fd);
            if (SSL_accept(ssl) == 1) {
                if (mode_ == Mode::Echo) {
                    char buf[4096];
                    int n = 0;
                    while ((n = SSL_read(ssl, buf, sizeof buf)) > 0) {
                        SSL_write(ssl, buf, n);
                    }
                } else {
                    SSL_shutdown(ssl);
                }
            }
            SSL_free(ssl);
            ::close(fd);
            ERR_clear_error();
        }
    }

    Mode mode_;
    SSL_CTX* ctx_ = nullptr;
    int listen_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

/// Dirige o transporte como a sessão dirige: poll no descritor, `progress()`.
Step drive(TlsTransport& t, Step step, int budget_ms = 15000)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    while (step == Step::Pending && std::chrono::steady_clock::now() < until) {
        if (t.fd() >= 0) {
            pollfd p{t.fd(), static_cast<short>(POLLIN | (t.wants_write() ? POLLOUT : 0)), 0};
            (void)::poll(&p, 1, 50);
        } else {
            ::usleep(20000);
        }
        step = t.progress();
    }
    return step;
}

/// Lê até chegar `size` bytes ou o transporte morrer.
std::string read_some(TlsTransport& t, std::size_t size, int budget_ms = 5000)
{
    std::string out;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    char buf[256];
    while (out.size() < size && std::chrono::steady_clock::now() < until) {
        pollfd p{t.fd(), POLLIN, 0};
        (void)::poll(&p, 1, 50);
        const ssize_t n = t.recv(buf, sizeof buf);
        if (n < 0) {
            break;
        }
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

class Tls : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        Ca good("ca de bancada");
        good.save("tls-ca");
        good.emit("tls-server", "localhost", "DNS:localhost,IP:127.0.0.1");
        good.emit("tls-client", "A1B2C3D4E5F6", nullptr);

        Ca other("outra ca");
        other.save("tls-other-ca");
        other.emit("tls-stranger", "A1B2C3D4E5F6", nullptr);
    }

    static void TearDownTestSuite()
    {
        for (const char* name : {"tls-ca.pem", "tls-server.pem", "tls-server.key", "tls-client.pem",
                                 "tls-client.key", "tls-other-ca.pem", "tls-stranger.pem",
                                 "tls-stranger.key"}) {
            std::remove((dir() + "/" + name).c_str());
        }
        ::rmdir(dir().c_str());
    }

    Endpoint endpoint(std::uint16_t port, const std::string& host = "127.0.0.1")
    {
        Endpoint e;
        e.host = host;
        e.port = port;
        e.ca_cert = dir() + "/tls-ca.pem";
        e.client_cert = dir() + "/tls-client.pem";
        e.client_key = dir() + "/tls-client.key";
        return e;
    }
};

}  // namespace

TEST_F(Tls, AbreEFalaNosDoisSentidos)
{
    Server server(Server::Mode::Echo, dir() + "/tls-ca.pem", false);
    TlsTransport t;
    ASSERT_EQ(drive(t, t.open(endpoint(server.port()))), Step::Done) << t.detail();

    const std::string hello = "PINGREQ de mentira";
    ASSERT_EQ(t.send(hello.data(), hello.size()), static_cast<ssize_t>(hello.size()));
    EXPECT_EQ(read_some(t, hello.size()), hello);
}

TEST_F(Tls, NomeDoServidorConferidoPorNome)
{
    Server server(Server::Mode::Echo, dir() + "/tls-ca.pem", false);
    TlsTransport t;
    EXPECT_EQ(drive(t, t.open(endpoint(server.port(), "localhost"))), Step::Done) << t.detail();
}

TEST_F(Tls, CaErradaEServidorNaoConfiavel)
{
    Server server(Server::Mode::Echo, dir() + "/tls-ca.pem", false);
    TlsTransport t;
    Endpoint e = endpoint(server.port());
    e.ca_cert = dir() + "/tls-other-ca.pem";  // um servidor qualquer se passando pelo broker
    EXPECT_EQ(drive(t, t.open(e)), Step::Failed);
    EXPECT_EQ(t.failure(), Failure::TlsUntrusted) << t.detail();
}

TEST_F(Tls, NomeQueNaoBateEServidorNaoConfiavel)
{
    Server server(Server::Mode::Echo, dir() + "/tls-ca.pem", false);
    TlsTransport t;
    // O certificado do servidor vale para localhost/127.0.0.1; pedir outro IP
    // literal que também chega nele (127.0.0.2 é loopback) tem que falhar.
    Endpoint e = endpoint(server.port(), "127.0.0.2");
    const Step step = drive(t, t.open(e));
    EXPECT_EQ(step, Step::Failed);
    // 127.0.0.2 pode não aceitar conexão onde o servidor ouve só 127.0.0.1;
    // os dois desfechos provam que não se conectou a quem não devia.
    EXPECT_TRUE(t.failure() == Failure::TlsUntrusted || t.failure() == Failure::Connect)
        << dextro::iot::session::failure_name(t.failure());
}

TEST_F(Tls, CertificadoNossoRecusadoNoHandshakeTls12)
{
    Server server(Server::Mode::Echo, dir() + "/tls-other-ca.pem", /*tls12_only=*/true);
    TlsTransport t;
    EXPECT_EQ(drive(t, t.open(endpoint(server.port()))), Step::Failed);
    EXPECT_EQ(t.failure(), Failure::TlsRejected) << t.detail();
}

TEST_F(Tls, CertificadoNossoRecusadoDepoisDoHandshakeTls13)
{
    Server server(Server::Mode::Echo, dir() + "/tls-other-ca.pem", /*tls12_only=*/false);
    TlsTransport t;
    const Step step = drive(t, t.open(endpoint(server.port())));
    if (step == Step::Done) {
        // TLS 1.3: o "pronto" do cliente vem antes da recusa; ela chega aqui.
        (void)read_some(t, 1);
    }
    EXPECT_EQ(t.failure(), Failure::TlsRejected) << t.detail();
}

TEST_F(Tls, BrokerQueFechaEPeerClosed)
{
    Server server(Server::Mode::CloseAfterHello, dir() + "/tls-ca.pem", false);
    TlsTransport t;
    ASSERT_EQ(drive(t, t.open(endpoint(server.port()))), Step::Done) << t.detail();
    (void)read_some(t, 1);
    EXPECT_EQ(t.failure(), Failure::PeerClosed) << t.detail();
}

TEST_F(Tls, EscreverDepoisQueOBrokerFechouNaoMataOProcesso)
{
    // Sem o BIO com MSG_NOSIGNAL, isto seria SIGPIPE — e o teste morreria
    // junto, que é exatamente o que aconteceria com o ssat-trafficd.
    Server server(Server::Mode::CloseAfterHello, dir() + "/tls-ca.pem", false);
    TlsTransport t;
    ASSERT_EQ(drive(t, t.open(endpoint(server.port()))), Step::Done) << t.detail();
    ::usleep(200000);  // o servidor já fechou
    const std::string data(4096, 'x');
    ssize_t rc = 0;
    for (int i = 0; i < 50 && rc >= 0; ++i) {
        rc = t.send(data.data(), data.size());
        ::usleep(10000);
    }
    EXPECT_LT(rc, 0);
    EXPECT_TRUE(t.failure() == Failure::Io || t.failure() == Failure::PeerClosed)
        << dextro::iot::session::failure_name(t.failure());
}

TEST_F(Tls, PortaFechadaEConnect)
{
    std::uint16_t port = 0;
    {
        Server server(Server::Mode::Echo, dir() + "/tls-ca.pem", false);
        port = server.port();
    }  // o servidor saiu; a porta recusa
    TlsTransport t;
    EXPECT_EQ(drive(t, t.open(endpoint(port))), Step::Failed);
    EXPECT_EQ(t.failure(), Failure::Connect) << t.detail();
}

TEST_F(Tls, CredencialAusenteNemTentaConectar)
{
    TlsTransport t;
    Endpoint e = endpoint(1);
    e.client_key = dir() + "/nao-existe.key";
    EXPECT_EQ(t.open(e), Step::Failed);
    EXPECT_EQ(t.failure(), Failure::Credentials);
}

TEST_F(Tls, ChaveQueNaoEDoCertificadoECredencial)
{
    TlsTransport t;
    Endpoint e = endpoint(1);
    e.client_key = dir() + "/tls-stranger.key";
    EXPECT_EQ(t.open(e), Step::Failed);
    EXPECT_EQ(t.failure(), Failure::Credentials);
}

TEST_F(Tls, DnsQueFalhaUsaOUltimoEnderecoBom)
{
    Server server(Server::Mode::Echo, dir() + "/tls-ca.pem", false);
    TlsTransport t;
    ASSERT_EQ(drive(t, t.open(endpoint(server.port(), "localhost"))), Step::Done) << t.detail();
    t.close();

    // `.invalid` nunca resolve (RFC 6761). O transporte cai no endereço da
    // última conexão boa — e o TLS recusa, porque o nome pedido não é o do
    // certificado: o cache ajuda a chegar, nunca a aceitar quem não devia.
    const Step step = drive(t, t.open(endpoint(server.port(), "broker.nao-existe.invalid")),
                            TlsTransport::kResolveTimeoutMs + 5000);
    EXPECT_EQ(step, Step::Failed);
    EXPECT_EQ(t.failure(), Failure::TlsUntrusted)
        << "chegou pelo cache e parou no nome: " << t.detail();
}

TEST_F(Tls, DnsQueFalhaSemCacheEResolve)
{
    TlsTransport t;
    const Step step = drive(t, t.open(endpoint(8883, "broker.nao-existe.invalid")),
                            TlsTransport::kResolveTimeoutMs + 5000);
    EXPECT_EQ(step, Step::Failed);
    EXPECT_EQ(t.failure(), Failure::Resolve) << t.detail();
}
