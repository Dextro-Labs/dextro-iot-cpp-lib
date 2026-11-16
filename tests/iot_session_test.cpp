#include <dextro-iot/session/backoff.hpp>
#include <dextro-iot/session/mqtt_session.hpp>

#include <gtest/gtest.h>

#include <mqtt.h>

#include <cstdlib>
#include <ctime>

#include <cstdint>
#include <string>
#include <vector>

using dextro::iot::session::Backoff;
using dextro::iot::session::Drop;
using dextro::iot::session::Endpoint;
using dextro::iot::session::Failure;
using dextro::iot::session::MqttSession;
using dextro::iot::session::SessionConfig;
using dextro::iot::session::Transport;

namespace {

// --- um broker MQTT 3.1.1 de bancada, dentro de um Transport em memória ------
//
// Ele lê o que o cliente escreve, entende CONNECT, SUBSCRIBE, PUBLISH, PINGREQ
// e DISCONNECT, e responde como o EMQX responderia — ou deixa de responder,
// ou fecha, ou recusa, conforme o teste manda. É assim que cada queda é
// provocada sem rede e sem esperar relógio de verdade.

class Packet {
public:
    std::uint8_t type = 0;
    std::uint8_t flags = 0;
    std::string body;
};

class Published {
public:
    std::string topic;
    std::string payload;
    int qos = 0;
    bool retain = false;
};

std::string remaining_length(std::size_t size)
{
    std::string out;
    do {
        auto byte = static_cast<std::uint8_t>(size % 128);
        size /= 128;
        if (size > 0) {
            byte |= 0x80;
        }
        out.push_back(static_cast<char>(byte));
    } while (size > 0);
    return out;
}

std::string u16(std::size_t v)
{
    return {static_cast<char>((v >> 8) & 0xff), static_cast<char>(v & 0xff)};
}

std::size_t read_u16(const std::string& s, std::size_t at)
{
    return (static_cast<std::size_t>(static_cast<std::uint8_t>(s[at])) << 8) |
           static_cast<std::uint8_t>(s[at + 1]);
}

std::string read_string(const std::string& s, std::size_t& at)
{
    const std::size_t size = read_u16(s, at);
    std::string out = s.substr(at + 2, size);
    at += 2 + size;
    return out;
}

class FakeBroker final : public Transport {
public:
    // --- o roteiro -----------------------------------------------------------
    Step open_step = Step::Pending;
    int progress_pending = 1;  ///< quantos `progress()` Pending antes do Done
    Step progress_final = Step::Done;
    Failure fail_as = Failure::Connect;

    bool send_connack = true;
    int connack_code = 0;
    bool answer_pings = true;
    bool subscribe_ok = true;
    bool fail_writes = false;

    // --- o que aconteceu -----------------------------------------------------
    int opens = 0;
    int connects = 0;
    int pings = 0;
    int disconnects = 0;
    std::string client_id;
    std::string will_topic;
    std::string will_message;
    bool will_retain = false;
    std::vector<std::string> subscriptions;
    std::vector<Published> published;

    // --- controle do fio -----------------------------------------------------
    void peer_close()
    {
        closed_by_peer_ = true;
    }

    void inject_publish(const std::string& topic, const std::string& payload)
    {
        std::string body = u16(topic.size()) + topic + payload;
        to_client_ += static_cast<char>(0x30);
        to_client_ += remaining_length(body.size());
        to_client_ += body;
    }

    // --- Transport -----------------------------------------------------------
    Step open(const Endpoint& endpoint) override
    {
        (void)endpoint;
        ++opens;
        open_ = true;
        pending_left_ = progress_pending;
        to_client_.clear();
        from_client_.clear();
        closed_by_peer_ = false;
        failure_ = open_step == Step::Failed ? fail_as : Failure::None;
        if (open_step == Step::Failed) {
            open_ = false;
        }
        return open_step;
    }

    Step progress() override
    {
        if (!open_) {
            return Step::Failed;
        }
        if (pending_left_ > 0) {
            --pending_left_;
            return Step::Pending;
        }
        if (progress_final == Step::Failed) {
            failure_ = fail_as;
            open_ = false;
        }
        return progress_final;
    }

    ssize_t send(const void* data, std::size_t size) override
    {
        if (!open_ || fail_writes) {
            failure_ = Failure::Io;
            detail_ = "EPIPE";
            return -1;
        }
        from_client_.append(static_cast<const char*>(data), size);
        serve();
        return static_cast<ssize_t>(size);
    }

    ssize_t recv(void* data, std::size_t size) override
    {
        if (!open_) {
            return -1;
        }
        if (to_client_.empty()) {
            if (closed_by_peer_) {
                failure_ = Failure::PeerClosed;
                detail_ = "FIN";
                return -1;
            }
            return 0;
        }
        const std::size_t n = std::min(size, to_client_.size());
        to_client_.copy(static_cast<char*>(data), n);
        to_client_.erase(0, n);
        return static_cast<ssize_t>(n);
    }

    void close() noexcept override { open_ = false; }
    int fd() const noexcept override { return -1; }
    bool wants_write() const noexcept override { return false; }
    Failure failure() const noexcept override { return failure_; }
    const std::string& detail() const noexcept override { return detail_; }

private:
    bool next_packet(Packet& out)
    {
        if (from_client_.size() < 2) {
            return false;
        }
        std::size_t length = 0;
        std::size_t multiplier = 1;
        std::size_t at = 1;
        for (;; ++at) {
            if (at >= from_client_.size()) {
                return false;
            }
            const auto byte = static_cast<std::uint8_t>(from_client_[at]);
            length += (byte & 0x7f) * multiplier;
            multiplier *= 128;
            if ((byte & 0x80) == 0) {
                break;
            }
        }
        if (from_client_.size() < at + 1 + length) {
            return false;
        }
        out.type = static_cast<std::uint8_t>(static_cast<std::uint8_t>(from_client_[0]) >> 4);
        out.flags = static_cast<std::uint8_t>(from_client_[0] & 0x0f);
        out.body = from_client_.substr(at + 1, length);
        from_client_.erase(0, at + 1 + length);
        return true;
    }

    void serve()
    {
        Packet p;
        while (next_packet(p)) {
            switch (p.type) {
            case 1: {  // CONNECT
                ++connects;
                std::size_t at = 0;
                (void)read_string(p.body, at);  // "MQTT"
                at += 1;                        // nível do protocolo
                const auto flags = static_cast<std::uint8_t>(p.body[at]);
                at += 1 + 2;  // flags + keepalive
                client_id = read_string(p.body, at);
                if ((flags & 0x04) != 0) {
                    will_topic = read_string(p.body, at);
                    will_message = read_string(p.body, at);
                    will_retain = (flags & 0x20) != 0;
                }
                if (send_connack) {
                    to_client_ += std::string("\x20\x02\x00", 3);
                    to_client_ += static_cast<char>(connack_code);
                }
                break;
            }
            case 3: {  // PUBLISH
                Published pub;
                pub.qos = (p.flags >> 1) & 0x03;
                pub.retain = (p.flags & 0x01) != 0;
                std::size_t at = 0;
                pub.topic = read_string(p.body, at);
                std::size_t id = 0;
                if (pub.qos > 0) {
                    id = read_u16(p.body, at);
                    at += 2;
                }
                pub.payload = p.body.substr(at);
                published.push_back(pub);
                if (pub.qos == 1) {
                    to_client_ += std::string("\x40\x02", 2) + u16(id);
                }
                break;
            }
            case 8: {  // SUBSCRIBE
                const std::size_t id = read_u16(p.body, 0);
                std::size_t at = 2;
                std::string codes;
                while (at < p.body.size()) {
                    subscriptions.push_back(read_string(p.body, at));
                    at += 1;  // qos pedido
                    codes += subscribe_ok ? '\x01' : '\x80';
                }
                to_client_ += static_cast<char>(0x90);
                to_client_ += remaining_length(2 + codes.size());
                to_client_ += u16(id) + codes;
                break;
            }
            case 12:  // PINGREQ
                ++pings;
                if (answer_pings) {
                    to_client_ += std::string("\xd0\x00", 2);
                }
                break;
            case 14:  // DISCONNECT
                ++disconnects;
                break;
            default:
                break;
            }
        }
    }

    bool open_ = false;
    bool closed_by_peer_ = false;
    int pending_left_ = 0;
    std::string to_client_;
    std::string from_client_;
    Failure failure_ = Failure::None;
    std::string detail_ = "roteiro";
};

double fixed_random(void* state)
{
    return *static_cast<double*>(state);
}

class Session : public ::testing::Test {
protected:
    Session() : session(broker, Backoff(&fixed_random, &random), [this] { return wall; }) {}

    SessionConfig config()
    {
        SessionConfig c;
        c.endpoint.host = "mqtt.dextro.example";
        c.serial = "A1B2C3D4E5F6";
        c.recv_buffer = 4096;
        c.send_buffer = 4096;
        return c;
    }

    /// Avança o relógio falso em passos de `step` ms até `until`.
    void run_until(std::int64_t until, std::int64_t step = 100)
    {
        while (now < until) {
            now += step;
            session.tick(now);
        }
    }

    void connect()
    {
        session.start(config(), now);
        for (int i = 0; i < 10 && !session.connected(); ++i) {
            now += 10;
            session.tick(now);
        }
        ASSERT_TRUE(session.connected());
    }

    double random = 1.0;  ///< o sorteio do backoff no extremo de cima
    std::int64_t now = 1000;
    std::int64_t wall = 1790000000000;
    FakeBroker broker;
    MqttSession session;
};

}  // namespace

// --- o backoff -----------------------------------------------------------------

TEST(IotBackoff, DobraAteOTetoEOPisoNuncaEZero)
{
    double r = 1.0;
    Backoff backoff(&fixed_random, &r);
    std::int64_t last = 0;
    for (int i = 0; i < 12; ++i) {
        const std::int64_t delay = backoff.next_delay();
        EXPECT_GE(delay, last) << i;
        EXPECT_LE(delay, Backoff::kCapMs) << i;
        last = delay;
    }
    EXPECT_EQ(last, Backoff::kCapMs);

    r = 0.0;
    EXPECT_EQ(backoff.next_delay(), 500) << "sorteio no zero ainda espera o piso";
}

TEST(IotBackoff, JitterEspalhaDentroDoIntervalo)
{
    double r = 0.5;
    Backoff backoff(&fixed_random, &r);
    (void)backoff.next_delay();
    (void)backoff.next_delay();
    const std::int64_t third = backoff.next_delay();  // teto 4000
    EXPECT_EQ(third, 500 + (4000 - 500) / 2);
}

TEST(IotBackoff, ResetVoltaAoComeco)
{
    double r = 1.0;
    Backoff backoff(&fixed_random, &r);
    for (int i = 0; i < 5; ++i) {
        (void)backoff.next_delay();
    }
    backoff.reset();
    EXPECT_EQ(backoff.next_delay(), Backoff::kInitialMs);
}

TEST(IotBackoff, MuitasFalhasNaoEstouram)
{
    double r = 1.0;
    Backoff backoff(&fixed_random, &r);
    for (int i = 0; i < 1000; ++i) {
        const std::int64_t delay = backoff.next_delay();
        ASSERT_GT(delay, 0);
        ASSERT_LE(delay, Backoff::kCapMs);
    }
}

// --- abrir -------------------------------------------------------------------

TEST_F(Session, ConectaAssinaEPublicaPresenca)
{
    connect();
    EXPECT_EQ(broker.connects, 1);
    EXPECT_EQ(broker.client_id, "A1B2C3D4E5F6") << "clientId é o serialNumber (spec §8)";
    EXPECT_EQ(broker.will_topic, "atrio/A1B2C3D4E5F6/status");
    EXPECT_EQ(broker.will_message, R"({"online":false})");
    EXPECT_TRUE(broker.will_retain) << "LWT retido (spec §8)";

    ASSERT_EQ(broker.subscriptions.size(), 2U);
    EXPECT_EQ(broker.subscriptions[0], "atrio/A1B2C3D4E5F6/procedure");
    EXPECT_EQ(broker.subscriptions[1], "atrio/A1B2C3D4E5F6/service/reply");

    ASSERT_FALSE(broker.published.empty());
    const Published& presence = broker.published.front();
    EXPECT_EQ(presence.topic, "atrio/A1B2C3D4E5F6/status");
    EXPECT_EQ(presence.payload, R"({"online":true,"ts":1790000000000})");
    EXPECT_TRUE(presence.retain);
    EXPECT_EQ(presence.qos, 1);
}

TEST_F(Session, TransporteQueNaoAbreVaiParaBackoffETentaDeNovo)
{
    broker.open_step = Transport::Step::Failed;
    broker.fail_as = Failure::Connect;
    session.start(config(), now);
    session.tick(now);
    EXPECT_EQ(session.state(), MqttSession::State::Backoff);
    EXPECT_EQ(session.last_drop(), Drop::Transport);
    EXPECT_EQ(session.transport_failure(), Failure::Connect);
    EXPECT_EQ(broker.opens, 1);

    // random = 1.0: a primeira espera é exatamente o teto inicial, 1 s.
    run_until(now + 900);
    EXPECT_EQ(broker.opens, 1) << "cedo demais";
    run_until(now + 200);
    EXPECT_EQ(broker.opens, 2);
}

TEST_F(Session, AberturaQueNuncaTerminaEstouraOPrazo)
{
    broker.progress_pending = 1000000;  // SYN engolido, handshake travado
    session.start(config(), now);
    const std::int64_t began = now;
    while (session.last_drop() != Drop::OpenTimeout && now - began < 60000) {
        now += 100;
        session.tick(now);
    }
    EXPECT_EQ(session.last_drop(), Drop::OpenTimeout);
    EXPECT_LE(now - began, SessionConfig{}.open_timeout_ms + 200);
}

TEST_F(Session, ConnackQueNaoVemEstouraOPrazo)
{
    broker.send_connack = false;
    session.start(config(), now);
    const std::int64_t began = now;
    while (session.last_drop() != Drop::ConnackTimeout && now - began < 60000) {
        now += 100;
        session.tick(now);
    }
    EXPECT_EQ(session.last_drop(), Drop::ConnackTimeout);
    EXPECT_LE(now - began, SessionConfig{}.connack_timeout_ms + 300);
}

TEST_F(Session, ConnackNaoAutorizadoECredencial)
{
    broker.connack_code = 5;
    session.start(config(), now);
    run_until(now + 500, 10);
    EXPECT_EQ(session.last_drop(), Drop::Unauthorized);
    EXPECT_FALSE(session.connected());
}

TEST_F(Session, ConnackServidorIndisponivelNaoECredencial)
{
    // "Servidor indisponível" (3) não pode levar o device a jogar fora o
    // certificado operacional; só "não autorizado" (4/5) leva.
    broker.connack_code = 3;
    session.start(config(), now);
    run_until(now + 500, 10);
    EXPECT_EQ(session.last_drop(), Drop::Refused);
}

TEST_F(Session, CertificadoRecusadoNoTlsViraCredencial)
{
    broker.progress_final = Transport::Step::Failed;
    broker.fail_as = Failure::TlsRejected;
    session.start(config(), now);
    run_until(now + 200, 10);
    EXPECT_EQ(session.last_drop(), Drop::Unauthorized);
    EXPECT_EQ(session.transport_failure(), Failure::TlsRejected);
}

// --- cair --------------------------------------------------------------------

TEST_F(Session, BrokerFechaAConexao)
{
    connect();
    broker.peer_close();
    now += 100;
    session.tick(now);
    EXPECT_EQ(session.state(), MqttSession::State::Backoff);
    EXPECT_EQ(session.last_drop(), Drop::Transport);
    EXPECT_EQ(session.transport_failure(), Failure::PeerClosed);
    EXPECT_EQ(session.drops(), 1);
}

TEST_F(Session, EscritaQueFalhaDerrubaASessao)
{
    connect();
    broker.fail_writes = true;
    EXPECT_TRUE(session.publish("atrio/A1B2C3D4E5F6/x", "{}", 1, false));
    now += 100;
    session.tick(now);
    EXPECT_FALSE(session.connected());
    EXPECT_EQ(session.transport_failure(), Failure::Io);
}

TEST_F(Session, MeiaConexaoCaiEmKeepaliveMaisPrazoDoPing)
{
    connect();
    broker.answer_pings = false;  // o NAT do 4G expirou: nada volta
    const std::int64_t began = now;
    while (session.connected() && now - began < 300000) {
        now += 100;
        session.tick(now);
    }
    EXPECT_EQ(session.last_drop(), Drop::HalfOpen);
    const SessionConfig c;
    EXPECT_LE(now - began, c.keepalive_s * 1000 + c.ping_timeout_ms + 500)
        << "meia-conexão tem que cair em ~25 s, não nos ~15 min do TCP";
    EXPECT_GE(broker.pings, 1);
}

TEST_F(Session, PingRespondidoMantemASessaoAberta)
{
    connect();
    run_until(now + 5 * 60 * 1000);
    EXPECT_TRUE(session.connected());
    EXPECT_EQ(session.drops(), 0);
    // Um PING a cada 15 s de silêncio, pelo relógio monotônico — o relógio
    // real deste teste mal andou, então nenhum destes veio do MQTT-C.
    EXPECT_GE(broker.pings, 18);
    EXPECT_LE(broker.pings, 21);
}

TEST_F(Session, PingrespNoMesmoMilissegundoNaoViraMeiaConexao)
{
    // Broker perto (LAN, docker, EMQX no mesmo cluster): o PINGRESP chega no
    // MESMO milissegundo em que o PING saiu. Comparando carimbos
    // (`last_rx_ms > ping_sent_ms`) isso nunca valia, e a sessão caía como
    // meia-conexão 10 s depois, com o broker respondendo — visto no e2e.
    connect();
    const int before = broker.pings;
    while (broker.pings == before) {
        now += 100;
        session.tick(now);
    }
    session.tick(now);  // o PINGRESP lido sem o relógio andar
    run_until(now + 60000);
    EXPECT_TRUE(session.connected());
    EXPECT_EQ(session.drops(), 0);
}

TEST_F(Session, RelogioDeParedeQueSaltaNaoMexeNoKeepalive)
{
    connect();
    wall -= 24LL * 3600 * 1000;  // o GNSS acertou a hora para trás
    run_until(now + 60000);
    wall += 48LL * 3600 * 1000;  // e para frente
    run_until(now + 60000);
    EXPECT_TRUE(session.connected());
    EXPECT_GE(broker.pings, 7);
}

TEST_F(Session, AssinaturaRecusadaReabre)
{
    broker.subscribe_ok = false;  // ACL do EMQX recusando o tópico
    session.start(config(), now);
    run_until(now + 500, 10);
    EXPECT_FALSE(session.connected());
    EXPECT_EQ(session.last_drop(), Drop::Protocol);
}

TEST_F(Session, PacoteMaiorQueOBufferReabre)
{
    connect();
    broker.inject_publish("atrio/A1B2C3D4E5F6/procedure", std::string(8192, 'x'));
    now += 100;
    session.tick(now);
    EXPECT_FALSE(session.connected());
    EXPECT_EQ(session.last_drop(), Drop::Protocol);
}

// --- voltar ------------------------------------------------------------------

TEST_F(Session, RedeQueMudaEmBackoffTentaNaHora)
{
    broker.open_step = Transport::Step::Failed;
    session.start(config(), now);
    for (int i = 0; i < 6; ++i) {  // empurra o backoff para dezenas de segundos
        run_until(now + Backoff::kCapMs + 1000, 1000);
    }
    const int opens = broker.opens;
    ASSERT_EQ(session.state(), MqttSession::State::Backoff);

    broker.open_step = Transport::Step::Pending;  // o 4G voltou
    session.network_changed(now);
    now += 10;
    session.tick(now);
    EXPECT_EQ(broker.opens, opens + 1) << "não pode esperar o fim do backoff";
    run_until(now + 100, 10);
    EXPECT_TRUE(session.connected());
}

TEST_F(Session, RedeQueMudaComSessaoAbertaSondaEDerrubaSeOCaminhoMorreu)
{
    connect();
    run_until(now + 2000);
    broker.answer_pings = false;  // o IP mudou; o caminho antigo morreu
    const int pings = broker.pings;
    session.network_changed(now);
    EXPECT_EQ(broker.pings, pings + 1) << "a sonda sai na hora";

    const std::int64_t began = now;
    while (session.connected() && now - began < 60000) {
        now += 100;
        session.tick(now);
    }
    EXPECT_EQ(session.last_drop(), Drop::HalfOpen);
    EXPECT_LE(now - began, SessionConfig{}.ping_timeout_ms + 200)
        << "sem a sonda, só no próximo keepalive";
}

TEST_F(Session, RedeQueMudaComCaminhoBomNaoDerruba)
{
    connect();
    session.network_changed(now);
    run_until(now + 30000);
    EXPECT_TRUE(session.connected());
}

TEST_F(Session, BackoffSoZeraDepoisDeSessaoEstavel)
{
    connect();
    broker.peer_close();  // sessão tomada: cai logo depois de abrir
    now += 100;
    session.tick(now);
    EXPECT_EQ(session.failures(), 1);

    run_until(now + 3000, 10);  // reconecta
    ASSERT_TRUE(session.connected());
    broker.peer_close();
    now += 100;
    session.tick(now);
    EXPECT_EQ(session.failures(), 2) << "quem cai logo depois de abrir não zera o backoff";

    run_until(now + 5000, 10);
    ASSERT_TRUE(session.connected());
    run_until(now + Backoff::kStableMs + 1000);
    EXPECT_EQ(session.failures(), 0);
}

TEST_F(Session, RestartReabreNaHoraSemContarQueda)
{
    connect();
    session.restart(now);
    now += 10;
    session.tick(now);
    run_until(now + 100, 10);
    EXPECT_TRUE(session.connected());
    EXPECT_EQ(broker.connects, 2);
    EXPECT_EQ(session.drops(), 0);
    EXPECT_EQ(session.failures(), 0);
}

TEST_F(Session, StopPublicaOfflineRetidoAntesDoDisconnect)
{
    connect();
    session.stop(now);
    ASSERT_GE(broker.published.size(), 2U);
    const Published& last = broker.published.back();
    EXPECT_EQ(last.topic, "atrio/A1B2C3D4E5F6/status");
    EXPECT_EQ(last.payload.find(R"("online":false)"), 1U);
    EXPECT_TRUE(last.retain) << "DISCONNECT limpo cancela o LWT; o offline tem que ser nosso";
    EXPECT_EQ(broker.disconnects, 1);
    EXPECT_EQ(session.state(), MqttSession::State::Idle);
}

// --- mensagens ---------------------------------------------------------------

TEST_F(Session, MensagemChegaERespostaSaiNaMesmaVolta)
{
    std::vector<std::string> seen;
    session.on_message([&](std::string_view topic, std::string_view payload) {
        seen.emplace_back(std::string(topic) + " " + std::string(payload));
        EXPECT_TRUE(session.publish("atrio/A1B2C3D4E5F6/procedure/reply/pod-1", "resposta", 1, false));
    });
    connect();
    const std::size_t before = broker.published.size();

    broker.inject_publish("atrio/A1B2C3D4E5F6/procedure", "pedido");
    now += 10;
    session.tick(now);

    ASSERT_EQ(seen.size(), 1U);
    EXPECT_EQ(seen[0], "atrio/A1B2C3D4E5F6/procedure pedido");
    ASSERT_EQ(broker.published.size(), before + 1) << "a resposta não pode esperar outro tick";
    EXPECT_EQ(broker.published.back().topic, "atrio/A1B2C3D4E5F6/procedure/reply/pod-1");
}

TEST_F(Session, PublicarSemSessaoDevolveFalse)
{
    EXPECT_FALSE(session.publish("x", "y", 1, false));
}

TEST(IotSessionConfig, BufferDeRecepcaoCobreOLimiteDoBroker)
{
    // dextro-infra, argocd/manifests/emqx/10-config.yaml: mqtt.max_packet_size
    // = "512KB". Um pacote que o broker aceita e o device não consegue ler
    // derruba a sessão. Se o broker subir o limite, este teste tem que subir.
    constexpr std::size_t kEmqxMaxPacket = 512 * 1024;
    EXPECT_GE(SessionConfig{}.recv_buffer, kEmqxMaxPacket + 5);
}

TEST(Sessao, RelogioDoMqttcEMonotonicoENaoOdeParede)
{
    // O MQTT-C mede o reenvio de QoS 1 com MQTT_PAL_TIME(). Na placa sem RTC o
    // relógio de parede salta um ano quando o GNSS acerta a hora; o do MQTT-C
    // tem que ser o monotônico (drivers/iot/session/mqttc_pal.h).
    timespec mono{};
    ASSERT_EQ(::clock_gettime(CLOCK_MONOTONIC, &mono), 0);
    const auto pal = static_cast<std::int64_t>(MQTT_PAL_TIME());
    EXPECT_LE(std::abs(pal - static_cast<std::int64_t>(mono.tv_sec)), 1);
    // E não é o time(NULL): em qualquer máquina ligada há mais de alguns
    // segundos depois de 1970 os dois diferem por décadas.
    EXPECT_GT(std::abs(static_cast<std::int64_t>(::time(nullptr)) - pal), 1000000);
}
