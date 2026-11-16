#pragma once

#include <dextro-iot/session/backoff.hpp>
#include <dextro-iot/session/mqtt_link.hpp>
#include <dextro-iot/session/transport.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct mqtt_client;
struct mqtt_response_publish;

/// A sessão MQTT com o broker do Dextro IoT, do jeito que um controlador atrás
/// de 4G precisa: **ela volta sozinha, sempre, e percebe quando morreu**.
///
/// ## A máquina de estados
///
///   Idle ──start──▶ Opening ──fio pronto──▶ Handshake ──CONNACK 0──▶ Connected
///                     │                        │                        │
///                     └──── falha/prazo ───────┴──── queda/prazo ───────┘
///                                              ▼
///                                           Backoff ──prazo ou rede mudou──▶ Opening
///
/// ## As quedas que ela cobre, e como
///
/// | Queda | Como é percebida | Em quanto tempo |
/// |---|---|---|
/// | DNS fora / sem resposta | prazo do transporte; cai no cache de endereços | 8 s |
/// | SYN engolido (4G sem dados) | prazo por endereço, depois o próximo | 10 s |
/// | recusa, sem rota, rede inalcançável | erro do `connect` | na hora |
/// | handshake TLS que trava | prazo da abertura inteira | `open_timeout_ms` |
/// | certificado recusado / expirado | alerta TLS ou CONNACK 5 → `Unauthorized` | na hora |
/// | CONNACK que não vem | prazo | `connack_timeout_ms` |
/// | broker fecha (FIN) / reset (RST) | leitura ou escrita falha | na hora |
/// | meia-conexão (NAT do 4G expirou, cabo, torre) | PINGRESP que não volta | `keepalive + ping_timeout` |
/// | link parado com dado no buffer | `TCP_USER_TIMEOUT` no socket | 30 s |
/// | troca de IP / de rota / de interface | `network_changed()` → sonda com PING | `ping_timeout` |
/// | relógio de parede que salta (GNSS, NTP) | nada a perceber: tudo aqui é monotônico | — |
/// | pacote acima do `max_packet_size` do broker | não chega (o buffer cobre o limite do EMQX) | — |
/// | assinatura recusada (ACL) | SUBACK 0x80 → reabre com backoff | na hora |
/// | sessão tomada por outro cliente com o mesmo id | FIN do broker; backoff com jitter e sem zerar até estabilizar | na hora |
///
/// ## Presença (spec §8)
///
/// LWT retido em `atrio/{sn}/status` com `{"online":false}`, e
/// `{"online":true,"ts"}` retido a cada conexão. No `stop()` a sessão publica o
/// `online:false` ela mesma antes do DISCONNECT: um DISCONNECT limpo cancela o
/// LWT, e sem isso o device desligado de propósito ficaria "online" para sempre.
///
/// ## Threads
///
/// Nenhuma. Tudo roda na thread de quem chama `tick()`. O callback de
/// mensagem do MQTT-C roda DENTRO do `mqtt_sync`, com o mutex do cliente
/// preso — publicar de dentro dele trava a thread. Por isso a mensagem é
/// enfileirada e entregue depois que o `mqtt_sync` volta.
namespace dextro::iot::session {

/// Por que a sessão caiu, ou por que não abriu.
enum class Drop {
    None,
    Transport,       ///< ver `transport_failure()`
    OpenTimeout,     ///< DNS + TCP + TLS não terminaram no prazo
    ConnackTimeout,
    Unauthorized,    ///< CONNACK 4/5, ou o broker recusou o certificado no TLS
    Refused,         ///< CONNACK 1/2/3 (versão, id recusado, servidor indisponível)
    HalfOpen,        ///< PINGRESP não voltou
    Protocol,        ///< o MQTT-C recusou algo que chegou (ou buffer pequeno)
    Stopped,
};

const char* drop_name(Drop drop) noexcept;

class SessionConfig {
public:
    Endpoint endpoint;
    /// O `serialNumber` — clientId e `{sn}` dos tópicos (spec §8, §4).
    std::string serial;

    int keepalive_s = 15;  ///< spec §8: device 15 s
    int open_timeout_ms = 30000;
    int connack_timeout_ms = 15000;
    /// Quanto esperar o PINGRESP antes de dar a conexão por morta.
    int ping_timeout_ms = 10000;

    /// Tamanho dos buffers do MQTT-C.
    ///
    /// O de recepção tem que caber o MAIOR pacote que o broker aceita, e não o
    /// MAX_PAYLOAD da aplicação: o EMQX está com `mqtt.max_packet_size =
    /// 512KB` de propósito (dextro-infra, argocd/manifests/emqx/10-config.yaml),
    /// acima dos 256 KB da aplicação, para que um payload grande demais vire
    /// 413 do service e não queda de conexão. Um pacote maior que este buffer
    /// o MQTT-C não tem como ler: a sessão cai. Com `clean session` o QoS 1 em
    /// voo morre junto — não vira laço —, mas a mensagem se perde. Se o broker
    /// subir o limite, este sobe junto.
    ///
    /// O de envio cabe o MAX_PAYLOAD (256 KB) mais tópico e envelope: acima
    /// disso é o próprio service que recusaria.
    std::size_t recv_buffer = 512 * 1024 + 4 * 1024;
    std::size_t send_buffer = 320 * 1024;
};

class MqttSession {
public:
    enum class State { Idle, Backoff, Opening, Handshake, Connected };

    /// Uma mensagem que chegou, para quem é dono da sessão.
    using OnMessage = std::function<void(std::string_view topic, std::string_view payload)>;
    /// O relógio de parede, em ms, só para o `ts` da presença.
    using WallClock = std::function<std::int64_t()>;

    MqttSession(Transport& transport, Backoff backoff, WallClock wall);
    ~MqttSession();

    MqttSession(const MqttSession&) = delete;
    MqttSession& operator=(const MqttSession&) = delete;

    void on_message(OnMessage handler) { on_message_ = std::move(handler); }

    /// Começa a conectar (na próxima `tick`). Troca a configuração: é assim
    /// que um certificado novo ou um broker novo passam a valer.
    void start(SessionConfig config, std::int64_t now_ms);

    /// Encerra: `online:false` retido, DISCONNECT, fecha. Melhor esforço, sem
    /// esperar resposta — é chamado no caminho de desligamento.
    void stop(std::int64_t now_ms) noexcept;

    /// Fecha a sessão atual e reabre agora, sem backoff. `certificate-install`
    /// e `settings-write` do broker usam isto (via `start()` com config nova).
    void restart(std::int64_t now_ms);

    /// A rede mudou (endereço, rota, interface). Em backoff: tenta agora. Com
    /// sessão aberta: manda um PING e dá `ping_timeout` para ele voltar — se o
    /// caminho antigo morreu, a sessão cai já, e não minutos depois.
    void network_changed(std::int64_t now_ms);

    /// Avança a máquina. Devolve em quantos ms quer ser chamada de novo, no
    /// máximo (a thread também acorda antes, pelo descritor).
    int tick(std::int64_t now_ms);

    /// Publica. `false` quando não há sessão ou o buffer de envio encheu.
    bool publish(const std::string& topic, std::string_view payload, int qos, bool retain);

    State state() const noexcept { return state_; }
    bool connected() const noexcept { return state_ == State::Connected; }

    int fd() const noexcept { return transport_.fd(); }
    bool wants_write() const noexcept;

    Drop last_drop() const noexcept { return last_drop_; }
    Failure transport_failure() const noexcept { return last_failure_; }
    const std::string& last_detail() const noexcept { return last_detail_; }
    /// Quedas depois de conectado (não conta tentativa que não abriu).
    int drops() const noexcept { return drops_; }
    /// Tentativas que falharam seguidas, desde a última sessão estável.
    int failures() const noexcept { return backoff_.failures(); }

    std::string topic(std::string_view suffix) const;

private:
    void begin_open(std::int64_t now_ms);
    void begin_mqtt(std::int64_t now_ms);
    void on_connected(std::int64_t now_ms);
    void fail(Drop why, std::string detail, std::int64_t now_ms);
    bool pump(std::int64_t now_ms);
    void deliver();
    std::string presence(bool online) const;

    static void publish_callback(void** state, struct mqtt_response_publish* message);

    Transport& transport_;
    Backoff backoff_;
    WallClock wall_;
    OnMessage on_message_;

    SessionConfig config_;
    State state_ = State::Idle;
    std::int64_t phase_started_ms_ = 0;
    std::int64_t retry_at_ms_ = 0;
    std::int64_t connected_at_ms_ = 0;
    bool stable_ = false;

    /// PING nosso em voo: quando saiu. 0 = nenhum.
    std::int64_t ping_sent_ms_ = 0;
    /// `link_.rx_events` quando o PING saiu: qualquer leitura depois disso
    /// prova o caminho vivo (ver o comentário em `dextro_iot_mqtt_link::rx_events`).
    std::uint64_t ping_rx_mark_ = 0;

    std::unique_ptr<mqtt_client> client_;
    dextro_iot_mqtt_link link_;
    std::vector<std::uint8_t> sendbuf_;
    std::vector<std::uint8_t> recvbuf_;
    std::string will_topic_;
    std::string will_message_;

    class Inbound {
    public:
        std::string topic;
        std::string payload;
    };
    std::vector<Inbound> inbox_;

    Drop last_drop_ = Drop::None;
    Failure last_failure_ = Failure::None;
    std::string last_detail_;
    int drops_ = 0;
};

}  // namespace dextro::iot::session
