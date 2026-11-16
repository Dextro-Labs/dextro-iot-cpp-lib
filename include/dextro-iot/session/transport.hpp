#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>

/// O fio por baixo da sessão MQTT: TCP + TLS, **não bloqueante do começo ao fim**.
///
/// Não bloquear é a regra que sustenta todo o resto. Um `connect` bloqueante
/// num 4G que engole SYN leva dois minutos para desistir; um `send` bloqueante
/// com o buffer cheio num link parado não volta nunca. Nos dois casos a thread
/// da nuvem fica presa, o keepalive não sai, e ninguém percebe que a sessão
/// morreu — nem o `stop()`, que espera o `join()`.
///
/// Então cada fase avança um passo por chamada e diz se terminou, se tem que
/// esperar o descritor, ou se falhou. Quem mede o tempo de cada fase é a
/// `MqttSession`, com o relógio monotônico dela.
///
/// Interface para a sessão rodar no CI contra um transporte em memória que
/// simula cada tipo de queda — que é onde as quedas são testadas de verdade.
namespace dextro::iot::session {

/// Onde conectar e com que identidade.
class Endpoint {
public:
    std::string host;
    std::uint16_t port = 8883;
    std::string ca_cert;      ///< caminho
    std::string client_cert;  ///< caminho
    std::string client_key;   ///< caminho
};

/// Por que o transporte caiu ou não abriu. Vai para o log e decide a política
/// do controlador (credencial recusada leva ao certificado de fábrica).
enum class Failure {
    None,
    Resolve,        ///< DNS não respondeu ou não achou o nome
    Connect,        ///< recusado, sem rota, ou todos os endereços falharam
    Tls,            ///< handshake falhou (sem ser por credencial)
    TlsRejected,    ///< o servidor recusou o NOSSO certificado (expirado, revogado)
    TlsUntrusted,   ///< o certificado do SERVIDOR não fecha com a CA ou o nome
    Credentials,    ///< arquivo de certificado/chave ausente ou ilegível
    PeerClosed,     ///< o outro lado fechou (FIN)
    Io,             ///< erro de socket: RST, EPIPE, timeout do TCP_USER_TIMEOUT
};

const char* failure_name(Failure failure) noexcept;

class Transport {
public:
    enum class Step {
        Done,      ///< a fase terminou
        Pending,   ///< esperar o descritor (ver `wants_write()`)
        Failed,    ///< ver `failure()` e `detail()`
    };

    virtual ~Transport() = default;

    /// Começa a abrir: resolução, socket, `connect` não bloqueante. Fecha o
    /// que houver aberto antes.
    virtual Step open(const Endpoint& endpoint) = 0;

    /// Avança a abertura (resolução → TCP → TLS). `Done` = pronto para MQTT.
    virtual Step progress() = 0;

    /// Escreve o que couber. `> 0` escritos, `0` = tentar depois, `< 0` =
    /// o transporte morreu (ver `failure()`). Nunca bloqueia, nunca SIGPIPE.
    virtual ssize_t send(const void* data, std::size_t size) = 0;

    /// Lê o que houver. `> 0` lidos, `0` = nada agora, `< 0` = morreu.
    virtual ssize_t recv(void* data, std::size_t size) = 0;

    virtual void close() noexcept = 0;

    /// O descritor para o `poll` da thread, ou -1.
    virtual int fd() const noexcept = 0;
    /// `true` quando a fase atual espera o descritor ficar gravável.
    virtual bool wants_write() const noexcept = 0;

    virtual Failure failure() const noexcept = 0;
    virtual const std::string& detail() const noexcept = 0;
};

}  // namespace dextro::iot::session
