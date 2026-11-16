#pragma once

#include <dextro-iot/clock.hpp>
#include <dextro-iot/remote_link.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>

/// O keepalive de aplicação: a remote-procedure `keepalive` (obrigatória,
/// SPEC do dextrolabs-device §4.4) a cada `intervalMs`.
///
/// ## Por que existe, se o MQTT já tem PING
///
/// O PING prova que o **broker** responde. O keepalive prova que o
/// **service** responde: o EMQX de pé com o iot-devices fora é uma sessão
/// "conectada" que não serve para nada. Duas sem resposta seguidas (504 ou
/// 5xx) com a sessão aberta e a sessão é reaberta — o limite dobra a cada
/// reabertura seguida por keepalive (até `kMissesCap`), para um service fora
/// por horas não virar um laço de reconexão, e volta a 2 no primeiro keepalive
/// respondido.
///
/// E é o keepalive que **ativa um certificado recém-instalado**: o service só
/// marca o `pending_install` como `active` quando um keepalive traz o
/// `certFingerprint` dele (§3.5). Por isso sai um já na abertura da sessão.
///
/// ## O payload
///
/// `additionalProperties: false` na raiz (§4.4): só os campos do
/// `KeepalivePayload` saem. O que é do tipo de device vai em `custom{}`
/// (validado pelo `device_type.keepalive_schema`); qualquer outra chave que a
/// aplicação puser em `Status::extra` é descartada com aviso, em vez de virar
/// um 400 em todo keepalive.
///
/// ## O que conta como resposta
///
///   - 503 é LOCAL (a sessão caiu ou foi reaberta com o pedido em voo): não
///     diz nada sobre o service, e a próxima sessão manda o seu;
///   - qualquer resposta do backend — até um 4xx — prova o service do outro
///     lado; o `nextIntervalMs` dela é respeitado, dentro de [15 s, 10 min];
///   - só o silêncio (504) e o 5xx contam como falha.
///
/// Origem: `src/services/iot-manager/iot_controller.cpp` (keepalive e
/// on_keepalive_reply) do ssat-traffic-controller. Uma thread: a do `tick()`.
namespace dextro::iot {

class Keepalive {
public:
    /// Intervalo sem orientação do backend, e os limites do `nextIntervalMs`.
    static constexpr std::int64_t kDefaultMs = 60000;
    static constexpr std::int64_t kMinMs = 15000;
    static constexpr std::int64_t kMaxMs = 600000;
    /// Keepalives sem resposta seguidos, com a sessão conectada, antes de
    /// reabrir a sessão. Dobra a cada reabertura seguida por keepalive (até
    /// `kMissesCap`).
    static constexpr int kMissesBeforeRestart = 2;
    static constexpr int kMissesCap = 32;

    /// O que a aplicação informa a cada keepalive. Tudo opcional.
    class Status {
    public:
        std::string fw_version;
        /// A versão de settings aplicada; < 0 = não informar.
        std::int64_t settings_version = -1;
        /// SHA-256 hex do certificado em uso (`certificate_fingerprint()`).
        std::string cert_fingerprint;
        /// Os outros campos padrão do §4.4: `rssi`, `freeMemBytes`, `cpuLoad`,
        /// `tempC`, `ip`, `batteryPct`. Chave fora da lista é descartada.
        nlohmann::json extra = nlohmann::json::object();
        /// O `custom{}` do tipo de device. Vazio = não sai.
        nlohmann::json custom = nlohmann::json::object();
    };

    using StatusFn = std::function<Status()>;
    /// A resposta do backend (`{serverTime, nextIntervalMs?, pendingOutbox?,
    /// settingsVersion?, settingsResync?}`), para a aplicação agir.
    using ReplyFn = std::function<void(const nlohmann::json& payload)>;

    /// O relógio monotônico em ms. Injetado: o teste anda o tempo à mão. A
    /// resposta chega num callback, fora do `tick()`, e é por ele que se sabe
    /// "agora" lá.
    using NowFn = std::int64_t (*)();

    /// `started_ms`: o instante da partida do processo (monotônico), base do
    /// `uptimeS`.
    Keepalive(RemoteLink& link, StatusFn status, std::int64_t started_ms,
              NowFn now = &clock::monotonic_ms);

    void on_reply(ReplyFn fn) { on_reply_ = std::move(fn); }

    /// Chamar a cada volta, depois do `Client::tick()`. Percebe a abertura de
    /// sessão sozinho (e manda um keepalive na hora).
    void tick(std::int64_t now_ms);

    /// Em quantos ms o próximo keepalive vence (para o teto do `poll`); -1
    /// sem sessão ou com um em voo.
    std::int64_t next_in(std::int64_t now_ms) const noexcept;

    /// O payload que sairia agora. Público para teste e diagnóstico.
    nlohmann::json payload(std::int64_t now_ms) const;

    std::int64_t interval_ms() const noexcept { return interval_ms_; }
    int misses() const noexcept { return misses_; }
    int restarts() const noexcept { return restarts_; }
    bool in_flight() const noexcept { return in_flight_; }

private:
    void reply(const RemoteResult& result);

    RemoteLink& link_;
    StatusFn status_;
    ReplyFn on_reply_;
    std::int64_t started_ms_;
    NowFn now_;

    bool was_connected_ = false;
    std::int64_t interval_ms_ = kDefaultMs;
    std::int64_t next_ms_ = 0;
    bool in_flight_ = false;
    int misses_ = 0;
    int restarts_ = 0;  ///< reaberturas seguidas por keepalive
    /// Incrementa a cada sessão: a resposta de um keepalive de uma sessão
    /// anterior não mexe no estado da atual.
    std::uint64_t generation_ = 0;
};

}  // namespace dextro::iot
