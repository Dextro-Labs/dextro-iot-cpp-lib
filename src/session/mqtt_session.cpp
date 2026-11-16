#include <dextro-iot/session/mqtt_session.hpp>

#include <dextro-iot/log.hpp>

#include <mqtt.h>

#include <algorithm>
#include <cstring>

namespace dextro::iot::session {
namespace {

/// Um PING nosso sai quando não houve envio há `keepalive`. O do MQTT-C fica
/// desligado: ele mede com `time(NULL)`, e um relógio de parede que volta no
/// tempo faria o PING nunca sair — e o broker derrubaria a sessão em 1,5×.
constexpr std::uint16_t kMqttcKeepaliveOff = 0xFFFF;

/// Quanto uma sessão tem que durar para uma queda em seguida não parecer
/// "outro cliente com o mesmo id tomou a sessão".
constexpr std::int64_t kFlapMs = 5000;

}  // namespace

const char* drop_name(Drop drop) noexcept
{
    switch (drop) {
    case Drop::None:
        return "nenhuma";
    case Drop::Transport:
        return "transporte";
    case Drop::OpenTimeout:
        return "abertura sem resposta";
    case Drop::ConnackTimeout:
        return "CONNACK sem resposta";
    case Drop::Unauthorized:
        return "credencial recusada";
    case Drop::Refused:
        return "conexão recusada pelo broker";
    case Drop::HalfOpen:
        return "meia-conexão (PINGRESP não voltou)";
    case Drop::Protocol:
        return "protocolo";
    case Drop::Stopped:
        return "encerrada";
    }
    return "desconhecida";
}

MqttSession::MqttSession(Transport& transport, Backoff backoff, WallClock wall)
    : transport_(transport), backoff_(backoff), wall_(std::move(wall))
{
}

MqttSession::~MqttSession()
{
    client_.reset();
    transport_.close();
}

std::string MqttSession::topic(std::string_view suffix) const
{
    std::string out = "atrio/";
    out += config_.serial;
    out += '/';
    out += suffix;
    return out;
}

std::string MqttSession::presence(bool online) const
{
    const std::int64_t ts = wall_ ? wall_() : 0;
    return std::string("{\"online\":") + (online ? "true" : "false") + ",\"ts\":" +
           std::to_string(ts) + "}";
}

void MqttSession::start(SessionConfig config, std::int64_t now_ms)
{
    config_ = std::move(config);
    sendbuf_.assign(config_.send_buffer, 0);
    recvbuf_.assign(config_.recv_buffer, 0);
    client_.reset();
    transport_.close();
    inbox_.clear();
    ping_sent_ms_ = 0;
    // Nenhuma espera na primeira tentativa.
    state_ = State::Backoff;
    retry_at_ms_ = now_ms;
}

void MqttSession::restart(std::int64_t now_ms)
{
    if (state_ == State::Idle) {
        return;
    }
    // Intencional: não conta como queda e não alimenta o backoff.
    client_.reset();
    transport_.close();
    inbox_.clear();
    state_ = State::Backoff;
    retry_at_ms_ = now_ms;
}

void MqttSession::stop(std::int64_t now_ms) noexcept
{
    try {
        if (state_ == State::Connected && client_) {
            // Um DISCONNECT limpo CANCELA o LWT. Sem este `online:false`
            // retido, o device desligado de propósito ficaria "online" no
            // broker até alguém apagar o retido à mão.
            const std::string offline = presence(false);
            (void)mqtt_publish(client_.get(), topic("status").c_str(), offline.data(), offline.size(),
                               MQTT_PUBLISH_QOS_1 | MQTT_PUBLISH_RETAIN);
            (void)mqtt_disconnect(client_.get());
            link_.now_ms = now_ms;
            (void)mqtt_sync(client_.get());
        }
    } catch (...) {
    }
    client_.reset();
    transport_.close();
    inbox_.clear();
    state_ = State::Idle;
    last_drop_ = Drop::Stopped;
}

void MqttSession::begin_open(std::int64_t now_ms)
{
    client_.reset();
    link_.reset(&transport_, now_ms);
    state_ = State::Opening;
    phase_started_ms_ = now_ms;

    switch (transport_.open(config_.endpoint)) {
    case Transport::Step::Done:
        begin_mqtt(now_ms);
        return;
    case Transport::Step::Pending:
        return;
    case Transport::Step::Failed:
        fail(Drop::Transport, transport_.detail(), now_ms);
        return;
    }
}

void MqttSession::begin_mqtt(std::int64_t now_ms)
{
    client_ = std::make_unique<mqtt_client>();
    std::memset(client_.get(), 0, sizeof(mqtt_client));
    link_.reset(&transport_, now_ms);

    enum MQTTErrors rc = mqtt_init(client_.get(), &link_, sendbuf_.data(), sendbuf_.size(),
                                   recvbuf_.data(), recvbuf_.size(), &MqttSession::publish_callback);
    if (rc != MQTT_OK) {
        fail(Drop::Protocol, mqtt_error_str(rc), now_ms);
        return;
    }
    client_->publish_response_callback_state = this;

    will_topic_ = topic("status");
    will_message_ = "{\"online\":false}";
    rc = mqtt_connect(client_.get(), config_.serial.c_str(), will_topic_.c_str(), will_message_.data(),
                      will_message_.size(), nullptr, nullptr,
                      MQTT_CONNECT_CLEAN_SESSION | MQTT_CONNECT_WILL_QOS_1 | MQTT_CONNECT_WILL_RETAIN,
                      static_cast<std::uint16_t>(config_.keepalive_s));
    if (rc != MQTT_OK) {
        fail(Drop::Protocol, mqtt_error_str(rc), now_ms);
        return;
    }
    // O CONNECT já foi montado com o keepalive certo; daqui em diante o
    // temporizador do MQTT-C fica fora do caminho (ver kMqttcKeepaliveOff).
    client_->keep_alive = kMqttcKeepaliveOff;

    state_ = State::Handshake;
    phase_started_ms_ = now_ms;
    (void)pump(now_ms);
}

void MqttSession::on_connected(std::int64_t now_ms)
{
    state_ = State::Connected;
    connected_at_ms_ = now_ms;
    stable_ = false;
    ping_sent_ms_ = 0;
    last_drop_ = Drop::None;

    // QoS 1 nas duas (spec §4); a sessão é limpa, então a assinatura é refeita
    // a cada conexão — e é aqui.
    (void)mqtt_subscribe(client_.get(), topic("procedure").c_str(), 1);
    (void)mqtt_subscribe(client_.get(), topic("service/reply").c_str(), 1);
    const std::string online = presence(true);
    (void)mqtt_publish(client_.get(), topic("status").c_str(), online.data(), online.size(),
                       MQTT_PUBLISH_QOS_1 | MQTT_PUBLISH_RETAIN);

    DEXTRO_IOT_INFO("dextro-iot: sessão aberta com %s:%u como %s", config_.endpoint.host.c_str(),
              config_.endpoint.port, config_.serial.c_str());
    (void)pump(now_ms);
}

void MqttSession::fail(Drop why, std::string detail, std::int64_t now_ms)
{
    const bool was_connected = state_ == State::Connected;
    client_.reset();
    transport_.close();
    inbox_.clear();
    ping_sent_ms_ = 0;

    last_drop_ = why;
    last_failure_ = why == Drop::Transport ? transport_.failure() : Failure::None;
    if (why == Drop::Transport && last_failure_ == Failure::TlsRejected) {
        // Para quem decide a política, recusa de certificado no TLS e CONNACK
        // "não autorizado" são a mesma coisa: a credencial não serve mais.
        last_drop_ = Drop::Unauthorized;
    }
    last_detail_ = std::move(detail);

    if (was_connected) {
        ++drops_;
    }
    const std::int64_t delay = backoff_.next_delay();
    state_ = State::Backoff;
    retry_at_ms_ = now_ms + delay;

    const char* what = last_drop_ == Drop::Transport ? failure_name(last_failure_) : drop_name(last_drop_);
    if (was_connected) {
        // Fechada pelo broker logo depois de abrir: o padrão de dois devices
        // com o mesmo serialNumber derrubando um ao outro.
        const bool flapped = now_ms - connected_at_ms_ < kFlapMs &&
                             last_failure_ == Failure::PeerClosed;
        DEXTRO_IOT_WARN("dextro-iot: sessão caiu após %llds: %s (%s)%s; nova tentativa em %lld ms",
                  static_cast<long long>((now_ms - connected_at_ms_) / 1000), what,
                  last_detail_.c_str(),
                  flapped ? " — logo depois de abrir: outro cliente com o mesmo id?" : "",
                  static_cast<long long>(delay));
    } else {
        DEXTRO_IOT_WARN("dextro-iot: não conectou: %s (%s); tentativa %d, próxima em %lld ms", what,
                  last_detail_.c_str(), backoff_.failures(), static_cast<long long>(delay));
    }
}

/// Uma volta de I/O do MQTT-C. `false` = a sessão caiu (e já foi tratada).
bool MqttSession::pump(std::int64_t now_ms)
{
    if (!client_) {
        return false;
    }
    link_.now_ms = now_ms;
    enum MQTTErrors rc = mqtt_sync(client_.get());

    if (rc == MQTT_OK && !link_.dead) {
        if (inbox_.empty()) {
            return true;
        }
        // As respostas que os handlers publicarem saem nesta mesma volta, e
        // não um tick depois.
        deliver();
        if (!client_) {
            return false;
        }
        rc = mqtt_sync(client_.get());
        if (rc == MQTT_OK && !link_.dead) {
            return true;
        }
    }

    // O CONNACK primeiro: uma recusa chega como erro do MQTT-C sem o código,
    // e o código é o que separa "credencial" de "broker indisponível".
    const int code = link_.connack_code();
    if (code > 0) {
        fail(code == 4 || code == 5 ? Drop::Unauthorized : Drop::Refused,
             "CONNACK " + std::to_string(code), now_ms);
        return false;
    }
    if (link_.dead || rc == MQTT_ERROR_SOCKET_ERROR) {
        fail(Drop::Transport, transport_.detail(), now_ms);
        return false;
    }
    fail(Drop::Protocol, mqtt_error_str(rc), now_ms);
    return false;
}

void MqttSession::deliver()
{
    if (inbox_.empty() || !on_message_) {
        inbox_.clear();
        return;
    }
    std::vector<Inbound> batch;
    batch.swap(inbox_);
    for (const Inbound& message : batch) {
        try {
            on_message_(message.topic, message.payload);
        } catch (...) {
            // Quem trata a mensagem não derruba a sessão.
        }
        if (!client_) {
            return;  // o handler pediu restart/stop
        }
    }
}

void MqttSession::publish_callback(void** state, struct mqtt_response_publish* message)
{
    if (state == nullptr || *state == nullptr || message == nullptr) {
        return;
    }
    auto* self = static_cast<MqttSession*>(*state);
    try {
        Inbound in;
        in.topic.assign(static_cast<const char*>(message->topic_name), message->topic_name_size);
        in.payload.assign(static_cast<const char*>(message->application_message),
                          message->application_message_size);
        self->inbox_.push_back(std::move(in));
    } catch (...) {
    }
}

bool MqttSession::publish(const std::string& topic_name, std::string_view payload, int qos, bool retain)
{
    if (state_ != State::Connected || !client_) {
        return false;
    }
    std::uint8_t flags = qos >= 1 ? MQTT_PUBLISH_QOS_1 : MQTT_PUBLISH_QOS_0;
    if (retain) {
        flags |= MQTT_PUBLISH_RETAIN;
    }
    return mqtt_publish(client_.get(), topic_name.c_str(), payload.data(), payload.size(), flags) ==
           MQTT_OK;
}

void MqttSession::network_changed(std::int64_t now_ms)
{
    switch (state_) {
    case State::Idle:
        return;
    case State::Backoff:
        // Esperar era por falta de caminho; o caminho mudou.
        backoff_.reset();
        retry_at_ms_ = now_ms;
        return;
    case State::Opening:
    case State::Handshake:
        // A tentativa em curso pode estar num caminho que acabou de sumir.
        begin_open(now_ms);
        return;
    case State::Connected:
        // A sonda: um PING agora. Se o caminho antigo morreu com a troca de
        // IP, a sessão cai em `ping_timeout` em vez de esperar o keepalive.
        if (ping_sent_ms_ == 0 && client_ && mqtt_ping(client_.get()) == MQTT_OK) {
            ping_sent_ms_ = now_ms;
            ping_rx_mark_ = link_.rx_events;
            (void)pump(now_ms);
        }
        return;
    }
}

bool MqttSession::wants_write() const noexcept
{
    return (state_ == State::Opening || state_ == State::Handshake || state_ == State::Connected) &&
           transport_.wants_write();
}

int MqttSession::tick(std::int64_t now_ms)
{
    switch (state_) {
    case State::Idle:
        return 1000;

    case State::Backoff:
        if (now_ms < retry_at_ms_) {
            return static_cast<int>(std::min<std::int64_t>(retry_at_ms_ - now_ms, 1000));
        }
        begin_open(now_ms);
        return 100;

    case State::Opening: {
        if (now_ms - phase_started_ms_ >= config_.open_timeout_ms) {
            fail(Drop::OpenTimeout, "DNS + TCP + TLS em mais de " +
                                        std::to_string(config_.open_timeout_ms / 1000) + " s",
                 now_ms);
            return 100;
        }
        switch (transport_.progress()) {
        case Transport::Step::Done:
            begin_mqtt(now_ms);
            break;
        case Transport::Step::Pending:
            break;
        case Transport::Step::Failed:
            fail(Drop::Transport, transport_.detail(), now_ms);
            break;
        }
        return 100;
    }

    case State::Handshake: {
        if (!pump(now_ms)) {
            return 100;
        }
        if (link_.connack_code() == 0) {
            on_connected(now_ms);
            return 100;
        }
        if (now_ms - phase_started_ms_ >= config_.connack_timeout_ms) {
            fail(Drop::ConnackTimeout, "sem CONNACK em " +
                                           std::to_string(config_.connack_timeout_ms / 1000) + " s",
                 now_ms);
        }
        return 100;
    }

    case State::Connected: {
        if (!pump(now_ms)) {
            return 100;
        }

        const std::int64_t keepalive_ms = std::int64_t{config_.keepalive_s} * 1000;

        // Qualquer coisa que chegou depois do PING prova que o caminho vive.
        // Contado em leituras, não comparado em milissegundos: o PINGRESP de
        // um broker perto chega no mesmo milissegundo do PING.
        if (ping_sent_ms_ != 0 && link_.rx_events > ping_rx_mark_) {
            ping_sent_ms_ = 0;
        }
        if (ping_sent_ms_ != 0 && now_ms - ping_sent_ms_ >= config_.ping_timeout_ms) {
            fail(Drop::HalfOpen,
                 "nada chegou em " + std::to_string((now_ms - link_.last_rx_ms) / 1000) + " s",
                 now_ms);
            return 100;
        }
        if (ping_sent_ms_ == 0 && now_ms - link_.last_tx_ms >= keepalive_ms) {
            if (mqtt_ping(client_.get()) == MQTT_OK) {
                ping_sent_ms_ = now_ms;
                ping_rx_mark_ = link_.rx_events;
                if (!pump(now_ms)) {
                    return 100;
                }
            }
        }
        // Cinto além do suspensório: nem PING conseguindo sair, nada chega há
        // duas voltas de keepalive. O TCP_USER_TIMEOUT pega isso também; este
        // não depende do kernel.
        if (now_ms - link_.last_rx_ms >= 2 * keepalive_ms + config_.ping_timeout_ms) {
            fail(Drop::HalfOpen,
                 "nada chegou em " + std::to_string((now_ms - link_.last_rx_ms) / 1000) + " s",
                 now_ms);
            return 100;
        }

        if (!stable_ && now_ms - connected_at_ms_ >= Backoff::kStableMs) {
            stable_ = true;
            backoff_.reset();
        }

        // O próximo compromisso: PING a sair, ou prazo do PING em voo.
        std::int64_t wait = keepalive_ms - (now_ms - link_.last_tx_ms);
        if (ping_sent_ms_ != 0) {
            wait = std::min<std::int64_t>(wait, config_.ping_timeout_ms - (now_ms - ping_sent_ms_));
        }
        return static_cast<int>(std::clamp<std::int64_t>(wait, 10, 1000));
    }
    }
    return 1000;
}

}  // namespace dextro::iot::session
