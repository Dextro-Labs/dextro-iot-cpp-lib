#include <dextro-iot/session/tls_transport.hpp>

#include <dextro-iot/clock.hpp>

// O ÚNICO lugar do projeto que fala TLS. O `certificate_store.cpp` usa o
// OpenSSL para gerar chave; este, para conectar.
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dextro::iot::session {
namespace {

std::int64_t now_ms() noexcept
{
    return clock::monotonic_ns() / 1000000;
}

// --- o BIO sem SIGPIPE -------------------------------------------------------
// O descritor mora no ponteiro de dados do BIO. `send`/`recv` com os flags
// certos, e o sinal de "tente de novo" do jeito que o OpenSSL espera.

int fd_of(BIO* bio) noexcept
{
    return static_cast<int>(reinterpret_cast<std::intptr_t>(BIO_get_data(bio)));
}

int bio_write(BIO* bio, const char* data, int size)
{
    BIO_clear_retry_flags(bio);
    const ssize_t n = ::send(fd_of(bio), data, static_cast<std::size_t>(size), MSG_NOSIGNAL);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        BIO_set_retry_write(bio);
        return -1;
    }
    return static_cast<int>(n);
}

int bio_read(BIO* bio, char* data, int size)
{
    BIO_clear_retry_flags(bio);
    const ssize_t n = ::recv(fd_of(bio), data, static_cast<std::size_t>(size), 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        BIO_set_retry_read(bio);
        return -1;
    }
    return static_cast<int>(n);
}

long bio_ctrl(BIO* bio, int cmd, long num, void* ptr)
{
    (void)bio;
    (void)num;
    (void)ptr;
    // O SSL pede FLUSH depois de escrever; o resto (kTLS, EOF, pending) não
    // se aplica a um socket cru e responde "não suportado".
    return cmd == BIO_CTRL_FLUSH ? 1 : 0;
}

int bio_create(BIO* bio)
{
    BIO_set_init(bio, 1);
    return 1;
}

BIO_METHOD* socket_method()
{
    static BIO_METHOD* const method = [] {
        BIO_METHOD* m = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "dextro-iot-socket");
        if (m != nullptr) {
            BIO_meth_set_write(m, bio_write);
            BIO_meth_set_read(m, bio_read);
            BIO_meth_set_ctrl(m, bio_ctrl);
            BIO_meth_set_create(m, bio_create);
        }
        return m;
    }();
    return method;
}

// --- a resolução assíncrona --------------------------------------------------

/// Um pedido ao `getaddrinfo_a`. Não se move de lugar depois de enviado: o
/// resolvedor da glibc escreve no `gaicb` por ponteiro, de outra thread.
class Lookup {
public:
    Lookup(const std::string& host_name, std::uint16_t port)
        : host(host_name), service(std::to_string(port))
    {
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_ADDRCONFIG;
        request.ar_name = host.c_str();
        request.ar_service = service.c_str();
        request.ar_request = &hints;
    }
    ~Lookup()
    {
        if (request.ar_result != nullptr) {
            freeaddrinfo(request.ar_result);
        }
    }
    Lookup(const Lookup&) = delete;
    Lookup& operator=(const Lookup&) = delete;

    std::string host;
    std::string service;
    addrinfo hints{};
    gaicb request{};
};

class Address {
public:
    sockaddr_storage storage{};
    socklen_t size = 0;

    bool same(const Address& other) const noexcept
    {
        return size == other.size && std::memcmp(&storage, &other.storage, size) == 0;
    }

    std::string text() const
    {
        char host[INET6_ADDRSTRLEN] = "?";
        std::uint16_t port = 0;
        if (storage.ss_family == AF_INET) {
            const auto* in = reinterpret_cast<const sockaddr_in*>(&storage);
            inet_ntop(AF_INET, &in->sin_addr, host, sizeof host);
            port = ntohs(in->sin_port);
        } else if (storage.ss_family == AF_INET6) {
            const auto* in = reinterpret_cast<const sockaddr_in6*>(&storage);
            inet_ntop(AF_INET6, &in->sin6_addr, host, sizeof host);
            port = ntohs(in->sin6_port);
        }
        return std::string(host) + ":" + std::to_string(port);
    }
};

/// O que o OpenSSL diz do último erro, em uma linha.
std::string ssl_error_text()
{
    const unsigned long code = ERR_peek_last_error();
    if (code == 0) {
        return errno != 0 ? std::string(std::strerror(errno)) : std::string("sem detalhe");
    }
    char text[256];
    ERR_error_string_n(code, text, sizeof text);
    return text;
}

/// O servidor mandou um alerta dizendo que o NOSSO certificado não serve.
bool rejected_our_certificate(unsigned long code) noexcept
{
    if (ERR_GET_LIB(code) != ERR_LIB_SSL) {
        return false;
    }
    switch (ERR_GET_REASON(code)) {
    case SSL_R_SSLV3_ALERT_BAD_CERTIFICATE:
    case SSL_R_SSLV3_ALERT_CERTIFICATE_EXPIRED:
    case SSL_R_SSLV3_ALERT_CERTIFICATE_REVOKED:
    case SSL_R_SSLV3_ALERT_CERTIFICATE_UNKNOWN:
    case SSL_R_TLSV1_ALERT_UNKNOWN_CA:
    case SSL_R_TLSV1_ALERT_ACCESS_DENIED:
    case SSL_R_TLSV13_ALERT_CERTIFICATE_REQUIRED:
        return true;
    default:
        return false;
    }
}

bool unexpected_eof(unsigned long code) noexcept
{
    return ERR_GET_LIB(code) == ERR_LIB_SSL && ERR_GET_REASON(code) == SSL_R_UNEXPECTED_EOF_WHILE_READING;
}

void tune_socket(int fd) noexcept
{
    const int on = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    (void)::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
    const int idle = 30;
    const int interval = 10;
    const int count = 3;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval);
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count);
    const unsigned int user_timeout = TlsTransport::kUserTimeoutMs;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout, sizeof user_timeout);
}

bool readable_file(const std::string& path) noexcept
{
    return !path.empty() && ::access(path.c_str(), R_OK) == 0;
}

}  // namespace

const char* failure_name(Failure failure) noexcept
{
    switch (failure) {
    case Failure::None:
        return "nenhuma";
    case Failure::Resolve:
        return "dns";
    case Failure::Connect:
        return "tcp";
    case Failure::Tls:
        return "tls";
    case Failure::TlsRejected:
        return "certificado recusado pelo broker";
    case Failure::TlsUntrusted:
        return "certificado do broker não confere";
    case Failure::Credentials:
        return "credencial local";
    case Failure::PeerClosed:
        return "fechada pelo broker";
    case Failure::Io:
        return "erro de socket";
    }
    return "desconhecida";
}

class TlsTransport::State {
public:
    enum class Phase { Closed, Resolving, Connecting, Handshake, Ready, Dead };

    ~State() { close_all(); }

    Step fail(Failure why, std::string text)
    {
        // Dead ANTES de fechar: depois de um erro fatal o OpenSSL proíbe o
        // SSL_shutdown, e o close_socket só o chama numa conexão Ready.
        phase = Phase::Dead;
        close_socket();
        failure = why;
        detail = std::move(text);
        return Step::Failed;
    }

    void close_socket() noexcept
    {
        if (ssl != nullptr) {
            // Um close_notify, se couber no buffer. Não espera resposta: é
            // cortesia com o broker, não requisito.
            if (phase == Phase::Ready) {
                ERR_clear_error();
                (void)SSL_shutdown(ssl);
            }
            SSL_free(ssl);  // libera o BIO junto
            ssl = nullptr;
        }
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
        write_blocked = false;
        want_write = false;
    }

    void close_all() noexcept
    {
        close_socket();
        cancel_lookup();
        reap(/*wait=*/false);
        if (ctx != nullptr) {
            SSL_CTX_free(ctx);
            ctx = nullptr;
        }
    }

    void cancel_lookup() noexcept
    {
        if (!lookup) {
            return;
        }
        if (gai_error(&lookup->request) == EAI_INPROGRESS &&
            gai_cancel(&lookup->request) == EAI_NOTCANCELED) {
            // O resolvedor ainda está escrevendo nele. Liberar agora seria uso
            // depois de liberar, na thread da glibc; fica guardado até acabar.
            abandoned.push_back(std::move(lookup));
        }
        lookup.reset();
    }

    /// Libera os pedidos de DNS abandonados que o resolvedor já soltou. Com
    /// `wait`, espera os outros — mas com prazo: um `gai_suspend` sem prazo
    /// prendia o destrutor, e com ele a recriação da thread da nuvem pelo
    /// supervisor e o stop do systemd. Passado o prazo, o pedido é VAZADO de
    /// propósito (`release`): a memória continua válida para a thread da glibc,
    /// que é o que importa; perder uns bytes por DNS travado é o preço.
    void reap(bool wait) noexcept
    {
        const std::int64_t deadline_ns = clock::monotonic_ns() + kReapWaitNs;
        for (auto it = abandoned.begin(); it != abandoned.end();) {
            if (wait) {
                const gaicb* list[] = {&(*it)->request};
                while (gai_error(&(*it)->request) == EAI_INPROGRESS) {
                    const std::int64_t left = deadline_ns - clock::monotonic_ns();
                    if (left <= 0) {
                        break;
                    }
                    timespec ts{};
                    ts.tv_sec = static_cast<time_t>(left / 1000000000);
                    ts.tv_nsec = static_cast<long>(left % 1000000000);
                    (void)gai_suspend(list, 1, &ts);
                }
                if (gai_error(&(*it)->request) == EAI_INPROGRESS) {
                    (void)it->release();  // vazado de propósito; ver acima
                }
                it = abandoned.erase(it);
            } else if (gai_error(&(*it)->request) != EAI_INPROGRESS) {
                it = abandoned.erase(it);
            } else {
                ++it;
            }
        }
    }

    /// Quanto o destrutor espera o resolvedor largar os pedidos abandonados.
    static constexpr std::int64_t kReapWaitNs = 5LL * 1000 * 1000 * 1000;

    Step load_context()
    {
        if (ctx != nullptr) {
            SSL_CTX_free(ctx);
            ctx = nullptr;
        }
        for (const std::string* path : {&endpoint.ca_cert, &endpoint.client_cert, &endpoint.client_key}) {
            if (!readable_file(*path)) {
                return fail(Failure::Credentials, "não lê " + (path->empty() ? std::string("(vazio)") : *path));
            }
        }

        ERR_clear_error();
        ctx = SSL_CTX_new(TLS_client_method());
        if (ctx == nullptr) {
            return fail(Failure::Tls, ssl_error_text());
        }
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        // Parcial + buffer móvel: é como o MQTT-C reenvia o que sobrou de um
        // envio parcial — do mesmo pacote, a partir de outro endereço.
        SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

        if (SSL_CTX_load_verify_locations(ctx, endpoint.ca_cert.c_str(), nullptr) != 1) {
            return fail(Failure::Credentials, "CA ilegível: " + ssl_error_text());
        }
        if (SSL_CTX_use_certificate_chain_file(ctx, endpoint.client_cert.c_str()) != 1) {
            return fail(Failure::Credentials, "certificado ilegível: " + ssl_error_text());
        }
        if (SSL_CTX_use_PrivateKey_file(ctx, endpoint.client_key.c_str(), SSL_FILETYPE_PEM) != 1) {
            return fail(Failure::Credentials, "chave ilegível: " + ssl_error_text());
        }
        if (SSL_CTX_check_private_key(ctx) != 1) {
            return fail(Failure::Credentials, "certificado e chave não formam par");
        }
        return Step::Done;
    }

    Step resolve()
    {
        const int status = gai_error(&lookup->request);
        if (status == EAI_INPROGRESS) {
            if (now_ms() - phase_started_ms < kResolveTimeoutMs) {
                return Step::Pending;
            }
            cancel_lookup();
            return use_cache("DNS sem resposta em " + std::to_string(kResolveTimeoutMs / 1000) + " s");
        }
        if (status != 0) {
            const std::string why = std::string("DNS: ") + gai_strerror(status);
            lookup.reset();
            return use_cache(why);
        }

        candidates.clear();
        for (const addrinfo* ai = lookup->request.ar_result; ai != nullptr; ai = ai->ai_next) {
            if (ai->ai_addrlen > sizeof(sockaddr_storage)) {
                continue;
            }
            Address address;
            std::memcpy(&address.storage, ai->ai_addr, ai->ai_addrlen);
            address.size = ai->ai_addrlen;
            // O endereço que funcionou da última vez vai na frente: com dois A
            // e um deles fora, não se paga o prazo do ruim a cada reconexão.
            if (!cache.empty() && address.same(cache.front())) {
                candidates.insert(candidates.begin(), address);
            } else {
                candidates.push_back(address);
            }
        }
        lookup.reset();
        if (candidates.empty()) {
            return use_cache("DNS sem endereço para " + endpoint.host);
        }
        next = 0;
        return connect_next();
    }

    Step use_cache(const std::string& why)
    {
        if (cache.empty()) {
            return fail(Failure::Resolve, why);
        }
        // DNS da operadora fora com o link de pé é comum no 4G. O IP de ontem
        // quase sempre ainda é o IP de hoje, e o TLS confere o nome do mesmo
        // jeito: conectar num endereço velho que virou outra coisa falha no
        // handshake, não conecta em quem não devia.
        candidates = cache;
        next = 0;
        tried_cache = true;
        cache_reason = why;
        return connect_next();
    }

    Step connect_next()
    {
        close_socket();
        while (next < candidates.size()) {
            current = candidates[next++];
            fd = ::socket(current.storage.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (fd < 0) {
                last_connect_error = std::strerror(errno);
                continue;
            }
            tune_socket(fd);
            const int rc =
                ::connect(fd, reinterpret_cast<const sockaddr*>(&current.storage), current.size);
            if (rc == 0 || errno == EINPROGRESS) {
                phase = Phase::Connecting;
                phase_started_ms = now_ms();
                want_write = true;
                return Step::Pending;
            }
            last_connect_error = current.text() + ": " + std::strerror(errno);
            ::close(fd);
            fd = -1;
        }
        std::string why = last_connect_error.empty() ? "sem endereço" : last_connect_error;
        if (tried_cache) {
            why += " (" + cache_reason + "; usou o último endereço bom)";
        }
        return fail(Failure::Connect, why);
    }

    Step finish_connect()
    {
        pollfd p{fd, POLLOUT, 0};
        const int ready = ::poll(&p, 1, 0);
        if (ready == 0) {
            if (now_ms() - phase_started_ms < kConnectTimeoutMs) {
                return Step::Pending;
            }
            last_connect_error = current.text() + ": sem resposta em " +
                                 std::to_string(kConnectTimeoutMs / 1000) + " s";
            return connect_next();
        }
        int error = 0;
        socklen_t size = sizeof error;
        if (ready < 0 || ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0) {
            last_connect_error = current.text() + ": " + std::strerror(error != 0 ? error : errno);
            return connect_next();
        }

        remember(current);
        return start_tls();
    }

    void remember(const Address& good)
    {
        for (auto it = cache.begin(); it != cache.end(); ++it) {
            if (it->same(good)) {
                cache.erase(it);
                break;
            }
        }
        cache.insert(cache.begin(), good);
        if (cache.size() > 4) {
            cache.resize(4);
        }
    }

    Step start_tls()
    {
        ERR_clear_error();
        ssl = SSL_new(ctx);
        BIO* bio = socket_method() != nullptr ? BIO_new(socket_method()) : nullptr;
        if (ssl == nullptr || bio == nullptr) {
            if (bio != nullptr) {
                BIO_free(bio);
            }
            return fail(Failure::Tls, ssl_error_text());
        }
        BIO_set_data(bio, reinterpret_cast<void*>(static_cast<std::intptr_t>(fd)));
        SSL_set_bio(ssl, bio, bio);

        // O nome que o certificado do broker tem que ter. Um IP literal no
        // `brokerUrl` é conferido como IP; um nome, como nome e como SNI.
        in6_addr probe{};
        const bool literal = inet_pton(AF_INET, endpoint.host.c_str(), &probe) == 1 ||
                             inet_pton(AF_INET6, endpoint.host.c_str(), &probe) == 1;
        if (literal) {
            X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), endpoint.host.c_str());
        } else {
            // O SNI. A macro SSL_set_tlsext_host_name faz um cast estilo C
            // que o -Werror recusa; esta é a mesma chamada, por extenso.
            (void)SSL_ctrl(ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name,
                           const_cast<char*>(endpoint.host.c_str()));
            SSL_set1_host(ssl, endpoint.host.c_str());
        }
        SSL_set_connect_state(ssl);

        phase = Phase::Handshake;
        return handshake();
    }

    Step handshake()
    {
        ERR_clear_error();
        errno = 0;
        const int rc = SSL_connect(ssl);
        if (rc == 1) {
            phase = Phase::Ready;
            want_write = false;
            return Step::Done;
        }
        const int error = SSL_get_error(ssl, rc);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            want_write = error == SSL_ERROR_WANT_WRITE;
            return Step::Pending;
        }

        const long verify = SSL_get_verify_result(ssl);
        if (verify != X509_V_OK) {
            return fail(Failure::TlsUntrusted,
                        std::string("broker em ") + current.text() + ": " +
                            X509_verify_cert_error_string(verify));
        }
        const unsigned long code = ERR_peek_last_error();
        if (rejected_our_certificate(code)) {
            return fail(Failure::TlsRejected, ssl_error_text());
        }
        return fail(Failure::Tls, current.text() + ": " + ssl_error_text());
    }

    /// O que um `SSL_read`/`SSL_write` que não deu certo significa.
    ssize_t io_result(int rc)
    {
        const int error = SSL_get_error(ssl, rc);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            return 0;
        }
        const unsigned long code = ERR_peek_last_error();
        if (rejected_our_certificate(code)) {
            // TLS 1.3: o cliente termina o handshake antes de o servidor
            // conferir o certificado dele. A recusa chega aqui, no primeiro
            // read depois do "pronto" — e é recusa de credencial do mesmo jeito.
            fail(Failure::TlsRejected, ssl_error_text());
            return -1;
        }
        if (error == SSL_ERROR_ZERO_RETURN || unexpected_eof(code) ||
            (error == SSL_ERROR_SYSCALL && errno == 0)) {
            fail(Failure::PeerClosed, "o broker fechou a conexão");
        } else {
            fail(Failure::Io, ssl_error_text());
        }
        return -1;
    }

    Endpoint endpoint;
    Phase phase = Phase::Closed;
    std::int64_t phase_started_ms = 0;

    std::unique_ptr<Lookup> lookup;
    std::vector<std::unique_ptr<Lookup>> abandoned;

    std::vector<Address> candidates;
    std::size_t next = 0;
    std::vector<Address> cache;  ///< os que já deram certo, o mais recente primeiro
    bool tried_cache = false;
    std::string cache_reason;
    Address current;
    std::string last_connect_error;

    int fd = -1;
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    bool want_write = false;
    bool write_blocked = false;

    Failure failure = Failure::None;
    std::string detail;
};

TlsTransport::TlsTransport() : state_(std::make_unique<State>()) {}

TlsTransport::~TlsTransport()
{
    state_->close_all();
    // Na destruição não dá para deixar um pedido de DNS órfão escrevendo em
    // memória que vai ser liberada: espera o que sobrou, com prazo (ver
    // `reap`), e vaza o que não voltou a tempo.
    state_->reap(/*wait=*/true);
}

Transport::Step TlsTransport::open(const Endpoint& endpoint)
{
    State& s = *state_;
    s.close_socket();
    s.cancel_lookup();
    s.reap(false);

    s.endpoint = endpoint;
    s.failure = Failure::None;
    s.detail.clear();
    s.tried_cache = false;
    s.last_connect_error.clear();

    if (s.load_context() == Step::Failed) {
        return Step::Failed;
    }

    s.lookup = std::make_unique<Lookup>(endpoint.host, endpoint.port);
    gaicb* list[] = {&s.lookup->request};
    const int rc = getaddrinfo_a(GAI_NOWAIT, list, 1, nullptr);
    if (rc != 0) {
        s.lookup.reset();
        s.phase_started_ms = now_ms();
        return s.use_cache(std::string("DNS: ") + gai_strerror(rc));
    }
    s.phase = State::Phase::Resolving;
    s.phase_started_ms = now_ms();
    return progress();
}

Transport::Step TlsTransport::progress()
{
    State& s = *state_;
    switch (s.phase) {
    case State::Phase::Resolving:
        return s.resolve();
    case State::Phase::Connecting:
        return s.finish_connect();
    case State::Phase::Handshake:
        return s.handshake();
    case State::Phase::Ready:
        return Step::Done;
    case State::Phase::Closed:
    case State::Phase::Dead:
        break;
    }
    if (s.failure == Failure::None) {
        s.failure = Failure::Io;
        s.detail = "transporte fechado";
    }
    return Step::Failed;
}

ssize_t TlsTransport::send(const void* data, std::size_t size)
{
    State& s = *state_;
    if (s.phase != State::Phase::Ready) {
        return -1;
    }
    ERR_clear_error();
    errno = 0;
    const int rc = SSL_write(s.ssl, data, static_cast<int>(size > INT_MAX ? INT_MAX : size));
    if (rc > 0) {
        s.write_blocked = false;
        return rc;
    }
    const ssize_t result = s.io_result(rc);
    s.write_blocked = result == 0;
    return result;
}

ssize_t TlsTransport::recv(void* data, std::size_t size)
{
    State& s = *state_;
    if (s.phase != State::Phase::Ready) {
        return -1;
    }
    ERR_clear_error();
    errno = 0;
    const int rc = SSL_read(s.ssl, data, static_cast<int>(size > INT_MAX ? INT_MAX : size));
    if (rc > 0) {
        return rc;
    }
    return s.io_result(rc);
}

void TlsTransport::close() noexcept
{
    State& s = *state_;
    s.close_socket();
    s.cancel_lookup();
    s.phase = State::Phase::Closed;
}

int TlsTransport::fd() const noexcept
{
    return state_->fd;
}

bool TlsTransport::wants_write() const noexcept
{
    const State& s = *state_;
    switch (s.phase) {
    case State::Phase::Connecting:
        return true;
    case State::Phase::Handshake:
        return s.want_write;
    case State::Phase::Ready:
        return s.write_blocked;
    default:
        return false;
    }
}

Failure TlsTransport::failure() const noexcept
{
    return state_->failure;
}

const std::string& TlsTransport::detail() const noexcept
{
    return state_->detail;
}

}  // namespace dextro::iot::session
