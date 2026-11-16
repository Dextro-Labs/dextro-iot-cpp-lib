#pragma once

#include <dextro-iot/session/transport.hpp>

#include <memory>
#include <vector>

/// TCP + TLS mútuo sobre OpenSSL 3, não bloqueante. O único `Transport` de
/// produção — não existe modo sem TLS: a spec §9 diz que TLS é MUST, e um
/// "modo de bancada" é o modo que alguém esquece ligado.
///
/// ## O que cada fase cobre
///
/// **Resolução** — `getaddrinfo_a`, assíncrona, com prazo próprio
/// (`kResolveTimeoutMs`). DNS de operadora de 4G some por minutos inteiros com
/// o link de pé; por isso os endereços da última conexão boa ficam guardados,
/// e uma resolução que falha ou estoura o prazo usa o cache em vez de desistir.
///
/// **TCP** — um endereço por vez, cada um com prazo (`kConnectTimeoutMs`): um
/// IPv6 anunciado mas sem rota não pode travar o IPv4 que funciona. No socket:
///
///   - `TCP_USER_TIMEOUT`: dado enviado e não confirmado por esse tempo derruba
///     a conexão. Sem ele, o kernel insiste por ~15 min (tcp_retries2) numa
///     conexão cujo NAT do 4G já expirou;
///   - keepalive de TCP: pega a meia-conexão também quando nada está sendo
///     enviado (a sessão MQTT tem o dela; este é o cinto além do suspensório);
///   - `TCP_NODELAY`: PINGREQ e respostas de procedure são pequenos.
///
/// **TLS** — TLS ≥ 1.2, CA pinada, nome do servidor conferido
/// (`SSL_set1_host`), certificado de cliente. Os arquivos são relidos a cada
/// abertura: é assim que um `certificate-install` passa a valer na reconexão.
///
/// **Sem SIGPIPE, em lugar nenhum.** O BIO de socket do OpenSSL escreve com
/// `write()`, e escrever num socket resetado mata o processo inteiro — este
/// processo, que dá o heartbeat ao coprocessador. Aqui o BIO é nosso e usa
/// `send(MSG_NOSIGNAL)`.
namespace dextro::iot::session {

class TlsTransport final : public Transport {
public:
    static constexpr int kResolveTimeoutMs = 8000;
    static constexpr int kConnectTimeoutMs = 10000;
    static constexpr int kUserTimeoutMs = 30000;

    TlsTransport();
    ~TlsTransport() override;

    TlsTransport(const TlsTransport&) = delete;
    TlsTransport& operator=(const TlsTransport&) = delete;

    Step open(const Endpoint& endpoint) override;
    Step progress() override;
    ssize_t send(const void* data, std::size_t size) override;
    ssize_t recv(void* data, std::size_t size) override;
    void close() noexcept override;

    int fd() const noexcept override;
    bool wants_write() const noexcept override;

    Failure failure() const noexcept override;
    const std::string& detail() const noexcept override;

private:
    class State;
    std::unique_ptr<State> state_;
};

}  // namespace dextro::iot::session
