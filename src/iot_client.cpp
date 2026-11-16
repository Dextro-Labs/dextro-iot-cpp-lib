#include <dextro-iot/iot_client.hpp>

#include <dextro-iot/procedures/envelope.hpp>
#include <dextro-iot/procedures/procedure_table.hpp>
#include <dextro-iot/procedures/remote_call.hpp>
#include <dextro-iot/session/mqtt_session.hpp>
#include <dextro-iot/session/tls_transport.hpp>
#include <dextro-iot/log.hpp>

#include <dextro-iot/clock.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <random>
#include <sys/random.h>
#include <sys/timex.h>

namespace dextro::iot {
namespace {

std::int64_t wall_ms()
{
    return clock::wall_ms();
}

/// O relógio de parede está sincronizado? É a pergunta do `timedatectl`
/// ("NTP synchronized"): o chrony limpa o `STA_UNSYNC` do kernel quando
/// sincroniza pelo GNSS ou pela rede (o mesmo vale para o systemd-timesyncd). `modes = 0` só lê; não pede privilégio.
bool clock_synchronized() noexcept
{
    timex tx{};
    const int state = ::adjtimex(&tx);
    return state != TIME_ERROR && (tx.status & STA_UNSYNC) == 0;
}

/// Um uuid v4 para o `messageId` da resposta.
std::string uuid_v4()
{
    unsigned char b[16] = {};
    if (::getrandom(b, sizeof b, 0) != static_cast<ssize_t>(sizeof b)) {
        std::random_device rd;
        for (unsigned char& byte : b) {
            byte = static_cast<unsigned char>(rd());
        }
    }
    b[6] = static_cast<unsigned char>((b[6] & 0x0f) | 0x40);
    b[8] = static_cast<unsigned char>((b[8] & 0x3f) | 0x80);
    char out[37];
    std::snprintf(out, sizeof out,
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                  b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14],
                  b[15]);
    return out;
}

double uniform(void* state)
{
    auto* engine = static_cast<std::mt19937*>(state);
    return std::uniform_real_distribution<double>(0.0, 1.0)(*engine);
}

std::mt19937 seeded_engine()
{
    std::uint32_t seed = 0;
    if (::getrandom(&seed, sizeof seed, 0) != static_cast<ssize_t>(sizeof seed)) {
        seed = std::random_device{}();
    }
    return std::mt19937(seed);
}

}  // namespace

std::string_view state_name(State state) noexcept
{
    switch (state) {
    case State::Disabled:
        return "desabilitado";
    case State::Disconnected:
        return "desconectado";
    case State::Connecting:
        return "conectando";
    case State::Connected:
        return "conectado";
    }
    return "desconhecido";
}

std::string canonical_serial(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == ':' || c == '-' || std::isspace(static_cast<unsigned char>(c)) != 0) {
            continue;
        }
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

bool Config::valid(std::string* why) const
{
    auto refuse = [why](const char* reason) {
        if (why != nullptr) {
            *why = reason;
        }
        return false;
    };
    // Sem host ou sem número de série, não há sessão possível: o SN é o
    // clientId e o `{sn}` de todos os tópicos.
    if (host.empty()) {
        return refuse("sem host do broker");
    }
    if (port == 0) {
        return refuse("porta 0");
    }
    // SPEC do dextrolabs-device §3.1: `^[0-9A-F]{12}$`. Um SN fora disso o
    // broker recusa (403 no MQTT) — para sempre, com backoff.
    if (serial.size() != 12 || !std::all_of(serial.begin(), serial.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
        })) {
        return refuse("serial fora de ^[0-9A-F]{12}$ (ver canonical_serial)");
    }
    // Não existe modo sem TLS (ver session/tls_transport.hpp).
    if (ca_cert.empty() || client_cert.empty() || client_key.empty()) {
        return refuse("sem CA, certificado ou chave do mTLS");
    }
    return true;
}

class Client::Impl {
public:
    Impl()
        : engine(seeded_engine()),
          session(transport, session::Backoff(&uniform, &engine), &wall_ms)
    {
        session.on_message([this](std::string_view topic, std::string_view payload) {
            on_message(topic, payload);
        });
    }

    void on_message(std::string_view topic, std::string_view payload)
    {
        if (topic == session.topic("procedure")) {
            on_procedure(payload);
        } else if (topic == session.topic("service/reply")) {
            // Resposta de remote-procedure: casa pelo relationId. Uma que não
            // casa chegou depois do prazo — o pedido já terminou com 504.
            if (!calls.on_reply(payload)) {
                DEXTRO_IOT_DEBUG("dextro-iot: resposta de remote-procedure sem pedido pendente");
            }
        }
    }

    /// A sessão caiu ou foi reaberta: quem esperava resposta recebe 503 já, em
    /// vez de esperar o prazo por uma resposta que não tem mais por onde voltar.
    void track_session(const char* why)
    {
        const bool connected = session.connected();
        if (was_connected && !connected) {
            calls.fail_all(503, why);
        }
        was_connected = connected;
    }

    void on_procedure(std::string_view text)
    {
        const std::string& serial = config.serial;
        procedures::Clock clock;
        clock.wall_ms = wall_ms();
        clock.synchronized = clock_known ? clock_ok : clock_synchronized();
        if (clock.synchronized != ttl_enabled) {
            // A spec §8: relógio não sincronizado desliga o TTL, com WARN.
            ttl_enabled = clock.synchronized;
            if (ttl_enabled) {
                DEXTRO_IOT_INFO("dextro-iot: relógio sincronizado; TTL de %lld s ligado",
                          static_cast<long long>(procedures::kRequestTtlMs / 1000));
            } else {
                DEXTRO_IOT_WARN("dextro-iot: relógio não sincronizado; TTL das device-procedures "
                          "desligado até o GNSS/NTP acertar a hora");
            }
        }
        procedures::Parsed parsed = procedures::parse_request(text, serial, clock);
        if (!parsed.replyable) {
            DEXTRO_IOT_WARN("dextro-iot: device-procedure descartada: %s", parsed.problem.c_str());
            return;
        }

        procedures::Reply reply;
        if (parsed.refusal) {
            reply = std::move(*parsed.refusal);
        } else if (text.size() > procedures::ProcedureTable::kMaxPayload) {
            // MAX_PAYLOAD da spec §9. O broker deixa passar até o limite dele
            // (maior, de propósito) justamente para esta resposta poder sair.
            reply = procedures::Reply::error(procedures::Rescode::PayloadTooLarge,
                                             procedures::ErrorCode::InvalidArgs,
                                             std::to_string(text.size()) + " bytes");
        } else if (table == nullptr || control == nullptr) {
            reply = procedures::Reply::error(procedures::Rescode::ServerError,
                                             procedures::ErrorCode::Unavailable, "tabela");
        } else {
            reply = table->dispatch_payload(parsed.envelope.op, std::move(parsed.envelope.payload),
                                            parsed.envelope.relation_id, control->bootstrap_scope());
        }
        if (!procedures::success(reply.rescode)) {
            DEXTRO_IOT_WARN("dextro-iot: %s -> %d %s", parsed.envelope.op.c_str(),
                      procedures::value(reply.rescode),
                      reply.payload.value("error", std::string{}).c_str());
        }

        const std::string body =
            procedures::build_reply(parsed.envelope, reply, serial, uuid_v4(), wall_ms());
        if (!session.publish(procedures::reply_topic(serial, parsed.envelope.instance), body, 1, false)) {
            DEXTRO_IOT_WARN("dextro-iot: resposta de %s não coube no buffer de envio (%zu bytes)",
                      parsed.envelope.op.c_str(), body.size());
        }
    }

    void track_failures(int failures_before)
    {
        if (session.connected()) {
            unauthorized_streak = 0;
            return;
        }
        if (session.failures() != failures_before &&
            session.last_drop() == session::Drop::Unauthorized) {
            ++unauthorized_streak;
        }
    }

    std::mt19937 engine;
    session::TlsTransport transport;
    session::MqttSession session;
    procedures::RemoteCalls calls;
    bool was_connected = false;

    Config config;
    bool running = false;
    procedures::ProcedureTable* table = nullptr;
    const procedures::SessionControl* control = nullptr;
    int unauthorized_streak = 0;
    /// Começa "ligado" para o primeiro pedido com relógio sem sincronia
    /// avisar no log.
    bool ttl_enabled = true;
    /// O veredito de `clock_synced()` (mesma thread do `tick`).
    bool clock_known = false;
    bool clock_ok = false;
};

Client::Client() : impl_(std::make_unique<Impl>()) {}

Client::~Client()
{
    stop(0);
}

void Client::bind(procedures::ProcedureTable& table, const procedures::SessionControl& session)
{
    impl_->table = &table;
    impl_->control = &session;
}

bool Client::start(const Config& config, std::int64_t now_ms)
{
    std::string why;
    if (!config.valid(&why)) {
        DEXTRO_IOT_ERR("dextro-iot: configuração recusada: %s", why.c_str());
        return false;
    }
    impl_->config = config;
    session::SessionConfig c;
    c.endpoint.host = config.host;
    c.endpoint.port = config.port;
    c.endpoint.ca_cert = config.ca_cert;
    c.endpoint.client_cert = config.client_cert;
    c.endpoint.client_key = config.client_key;
    c.serial = config.serial;
    impl_->calls.fail_all(503, "sessão reaberta");
    impl_->session.start(std::move(c), now_ms);
    impl_->was_connected = false;
    impl_->running = true;
    DEXTRO_IOT_INFO("dextro-iot: %s:%u como '%s'", config.host.c_str(), config.port, config.serial.c_str());
    return true;
}

void Client::stop(std::int64_t now_ms) noexcept
{
    if (!impl_->running) {
        return;
    }
    impl_->session.stop(now_ms);
    impl_->calls.fail_all(503, "sessão encerrada");
    impl_->was_connected = false;
    impl_->running = false;
}

int Client::tick(std::int64_t now_ms)
{
    const int before = impl_->session.failures();
    int wait = impl_->session.tick(now_ms);
    impl_->track_failures(before);
    impl_->track_session("sessão caiu");
    impl_->calls.expire(now_ms);
    const std::int64_t due = impl_->calls.next_deadline_in(now_ms);
    if (due >= 0 && due < wait) {
        wait = static_cast<int>(due);
    }
    return wait;
}

void Client::network_changed(std::int64_t now_ms)
{
    impl_->session.network_changed(now_ms);
}

void Client::clock_synced(bool synced)
{
    impl_->clock_known = true;
    impl_->clock_ok = synced;
}

void Client::restart(std::int64_t now_ms)
{
    if (!impl_->running) {
        return;
    }
    impl_->calls.fail_all(503, "sessão reaberta");
    impl_->session.restart(now_ms);
    impl_->was_connected = false;
}

void Client::call(std::string_view op, std::string_view payload_json, std::int64_t now_ms,
                  std::int64_t timeout_ms, RemoteDone done)
{
    auto deliver = [done = std::move(done)](const procedures::RemoteReply& r) {
        if (done) {
            RemoteResult out;
            out.rescode = r.rescode;
            out.payload_json = r.payload.dump();
            out.error = r.error;
            done(out);
        }
    };
    auto refuse = [&deliver](int rescode, const char* why) {
        procedures::RemoteReply r;
        r.rescode = rescode;
        r.error = why;
        deliver(r);
    };
    if (!impl_->running || !impl_->session.connected()) {
        refuse(503, "sem sessão");
        return;
    }
    procedures::RemoteCalls::Request rq;
    rq.op = std::string(op);
    rq.payload = procedures::Json::parse(payload_json, nullptr, /*allow_exceptions=*/false);
    if (rq.payload.is_discarded() || !rq.payload.is_object()) {
        refuse(400, "payload não é um objeto JSON");
        return;
    }
    rq.serial = impl_->config.serial;
    rq.message_id = uuid_v4();
    rq.relation_id = uuid_v4();
    rq.wall_ms = wall_ms();
    rq.now_ms = now_ms;
    rq.timeout_ms = timeout_ms > 0 ? timeout_ms : procedures::RemoteCalls::kDefaultTimeoutMs;

    const std::string body = impl_->calls.begin(rq, deliver);
    if (body.empty()) {
        return;  // recusado, e o done já rodou
    }
    if (body.size() > procedures::ProcedureTable::kMaxPayload) {
        impl_->calls.cancel(rq.relation_id, 413, "pedido acima do MAX_PAYLOAD");
        return;
    }
    if (!impl_->session.publish(impl_->session.topic("service"), body, 1, false)) {
        impl_->calls.cancel(rq.relation_id, 503, "buffer de envio cheio");
    }
}

bool Client::publish(std::string_view suffix, std::string_view payload, int qos)
{
    if (!impl_->running || !impl_->session.connected()) {
        return false;
    }
    return impl_->session.publish(impl_->session.topic(suffix), payload, qos, false);
}

std::string_view Client::last_drop_name() const noexcept
{
    const session::Drop drop = impl_->session.last_drop();
    if (drop == session::Drop::Transport) {
        return session::failure_name(impl_->session.transport_failure());
    }
    return session::drop_name(drop);
}

int Client::fd() const noexcept
{
    return impl_->session.fd();
}

bool Client::wants_write() const noexcept
{
    return impl_->session.wants_write();
}

bool Client::connected() const noexcept
{
    return impl_->running && impl_->session.connected();
}

State Client::state() const noexcept
{
    if (!impl_->running) {
        return State::Disabled;
    }
    switch (impl_->session.state()) {
    case session::MqttSession::State::Idle:
        return State::Disabled;
    case session::MqttSession::State::Backoff:
        return State::Disconnected;
    case session::MqttSession::State::Opening:
    case session::MqttSession::State::Handshake:
        return State::Connecting;
    case session::MqttSession::State::Connected:
        return State::Connected;
    }
    return State::Disconnected;
}

std::string_view Client::last_error() const noexcept
{
    return impl_->session.last_detail();
}

int Client::drops() const noexcept
{
    return impl_->session.drops();
}

int Client::unauthorized_streak() const noexcept
{
    return impl_->unauthorized_streak;
}

const std::string& Client::serial() const noexcept
{
    return impl_->config.serial;
}

}  // namespace dextro::iot
