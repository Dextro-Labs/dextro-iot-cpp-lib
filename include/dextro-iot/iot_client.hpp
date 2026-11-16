#pragma once

#include <dextro-iot/remote_link.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

/// A sessão com o Dextro Cloud: configuração, transporte e broker.
///
/// Fala o protocolo Dextro IoT (binding backend v0.5, SPEC do
/// dextrolabs-device §4) sobre MQTT com a `MqttSession` (`session/`), direto
/// sobre o MQTT-C e o OpenSSL. Nada de MQTT, TLS ou JSON aparece neste header.
///
/// Origem: `src/drivers/iot/iot_client.{hpp,cpp}` do ssat-traffic-controller
/// (o stub `IotClient` que esta biblioteca tinha antes nunca fez o handshake
/// MQTT e saiu). O `Config::from_parameters` (que lia o ssat.ini) ficou lá:
/// aqui quem preenche o `Config` é a aplicação.
///
/// ## O que esta classe faz
///
///   - mantém a sessão (a `MqttSession` cobre as quedas — ver a tabela dela);
///   - entrega cada device-procedure à `procedures::ProcedureTable` e publica a
///     resposta no envelope do protocolo;
///   - conta as recusas de credencial seguidas, para o controlador decidir
///     voltar ao certificado de fábrica.
///
/// Quem a dirige é UMA thread da aplicação: `tick()` em laço, com `poll()` no
/// `fd()` (e no do `LinkMonitor`). Nada aqui é thread-safe: um dono, uma
/// thread. Todo `now_ms` é do relógio monotônico (`clock::monotonic_ms()`).
namespace dextro::iot::procedures {
class ProcedureTable;
class SessionControl;
}  // namespace dextro::iot::procedures

namespace dextro::iot {

enum class State {
    Disabled,      ///< sem parâmetros semeados, ou parada
    Disconnected,  ///< fora do ar, esperando a próxima tentativa
    Connecting,    ///< DNS, TCP, TLS ou CONNACK em curso
    Connected,
};

std::string_view state_name(State state) noexcept;

class Config {
public:
    std::string host;
    std::uint16_t port = 8883;
    /// O `serialNumber`: clientId MQTT, CN do certificado e `{sn}` dos
    /// tópicos (spec §8; SPEC do dextrolabs-device: MQTT usa o SN, REST o id).
    std::string serial;
    std::string ca_cert;      ///< caminho do PEM da CA do broker
    std::string client_cert;  ///< caminho do PEM do certificado do device (CN = SN)
    std::string client_key;   ///< caminho do PEM da chave privada

    /// Confere o que dá para conferir sem rede: host, porta, os três caminhos
    /// e o SN no formato `^[0-9A-F]{12}$` (SPEC do dextrolabs-device §3.1).
    /// `false` = NÃO se deve tentar conectar: uma credencial vazia ou um SN
    /// torto no broker é uma recusa que se repete para sempre. `why` recebe o
    /// motivo.
    bool valid(std::string* why = nullptr) const;
};

/// O SN canônico (SPEC §3.1): `trim`, maiúsculas, sem `:` e `-`. Não valida —
/// quem valida é `Config::valid()`.
std::string canonical_serial(std::string_view text);

class Client final : public RemoteLink {
public:
    Client();
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /// Liga as device-procedures. `session` diz, a cada chamada, se a sessão
    /// está no escopo do certificado de fábrica. Os dois vivem mais que este.
    void bind(procedures::ProcedureTable& table, const procedures::SessionControl& session);

    /// (Re)abre com esta configuração, agora. Fecha a sessão anterior sem
    /// contar como queda: é assim que um certificado novo passa a valer.
    /// `false` (e nada muda) quando `config.valid()` recusa.
    bool start(const Config& config, std::int64_t now_ms);

    /// `online:false` retido, DISCONNECT, fecha.
    void stop(std::int64_t now_ms) noexcept;

    /// Avança. Devolve o teto de espera em ms.
    int tick(std::int64_t now_ms);

    /// Veio do `LinkMonitor`: a rede mudou.
    void network_changed(std::int64_t now_ms);

    /// O veredito do relógio de parede, quando a aplicação tem fonte melhor
    /// que o kernel (no controlador SSAT, o daemon do relógio, chrony + GNSS).
    /// Até o primeiro chegar, vale o kernel (`adjtimex`, `STA_UNSYNC`) — que
    /// numa placa sem `rtcsync` nunca diz sincronizado. Decide o TTL das
    /// device-procedures (`procedures/envelope.hpp`).
    void clock_synced(bool synced);

    /// Fecha a sessão e reabre agora, sem backoff e sem contar como queda —
    /// o que o keepalive de aplicação usa quando o service para de responder.
    void restart(std::int64_t now_ms) override;

    /// Chama uma remote-procedure do backend (`atrio/{sn}/service`). O `done`
    /// roda nesta mesma thread, dentro de um `tick()` futuro (ou já, quando
    /// recusado): 503 sem sessão, 504 no prazo, 413 grande demais, 429 fila
    /// cheia, ou o `rescode` que o backend devolveu.
    void call(std::string_view op, std::string_view payload_json, std::int64_t now_ms,
              std::int64_t timeout_ms, RemoteDone done) override;

    /// `state() == State::Connected`.
    bool connected() const noexcept override;

    /// Publica uma mensagem simples em `atrio/{sn}/{suffix}`, sem envelope de
    /// remote-procedure e sem esperar resposta (o `live`, SPEC §4.1). `false`
    /// sem sessão ou com o buffer de envio cheio: quem chama descarta, o
    /// próximo substitui. Nunca enfileira para depois.
    bool publish(std::string_view suffix, std::string_view payload, int qos);

    /// Por que a última sessão caiu (ou não abriu), no vocabulário da sessão.
    std::string_view last_drop_name() const noexcept;

    int fd() const noexcept;
    bool wants_write() const noexcept;

    State state() const noexcept;
    std::string_view last_error() const noexcept;

    /// Quedas depois de conectado, desde a partida. Um número que sobe sozinho
    /// num controlador parado é sinal de rede ruim.
    int drops() const noexcept;

    /// Tentativas seguidas em que o broker recusou a credencial (CONNACK 4/5,
    /// ou alerta TLS de certificado). Zera ao conectar.
    int unauthorized_streak() const noexcept;

    /// O SN da configuração atual (vazio antes do `start`).
    const std::string& serial() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dextro::iot
