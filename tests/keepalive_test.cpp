// O keepalive de aplicação (SPEC do dextrolabs-device §4.4) contra um backend
// falso. A lógica veio de src/services/iot-manager/iot_controller.cpp do
// ssat-traffic-controller, que não tinha teste próprio dela.
#include "fake_link.hpp"

#include <dextro-iot/keepalive.hpp>

#include <gtest/gtest.h>

using dextro::iot::Keepalive;
using nlohmann::json;

namespace {

std::int64_t g_now = 0;
std::int64_t fake_now()
{
    return g_now;
}

Keepalive::Status status()
{
    Keepalive::Status s;
    s.fw_version = "1.4.2";
    s.settings_version = 14;
    s.cert_fingerprint = std::string(64, 'a');
    s.extra = {{"rssi", -61}, {"tempC", 41.5}, {"door", 1}};
    s.custom = {{"printheadC", 55}};
    return s;
}

class KeepaliveTest : public ::testing::Test {
protected:
    void SetUp() override { g_now = 100000; }
    FakeLink link;
    Keepalive ka{link, &status, 0, &fake_now};
};

}  // namespace

TEST_F(KeepaliveTest, SaiUmNaAberturaComOsCamposDoParagrafo44)
{
    ka.tick(g_now);
    ASSERT_EQ(link.calls.size(), 1U);
    const auto& c = link.calls[0];
    EXPECT_EQ(c.op, "keepalive");
    EXPECT_EQ(c.payload["intervalMs"], Keepalive::kDefaultMs);
    EXPECT_EQ(c.payload["uptimeS"], 100);
    EXPECT_EQ(c.payload["fwVersion"], "1.4.2");
    EXPECT_EQ(c.payload["settingsVersion"], 14);
    EXPECT_EQ(c.payload["certFingerprint"], std::string(64, 'a'));
    EXPECT_EQ(c.payload["rssi"], -61);
    EXPECT_EQ(c.payload["custom"]["printheadC"], 55);
    // `additionalProperties: false`: o que não é do §4.4 não sai.
    EXPECT_FALSE(c.payload.contains("door"));
}

TEST_F(KeepaliveTest, UmPorVezEORespeitaONextIntervalMsDentroDosLimites)
{
    ka.tick(g_now);
    ka.tick(g_now + 1);
    ASSERT_EQ(link.calls.size(), 1U) << "um em voo por vez";
    link.answer_next(200, {{"serverTime", 1}, {"nextIntervalMs", 1000}});
    EXPECT_EQ(ka.interval_ms(), Keepalive::kMinMs) << "abaixo do piso: vale o piso";
    ka.tick(g_now + Keepalive::kMinMs - 1);
    EXPECT_EQ(link.calls.size(), 1U);
    ka.tick(g_now + Keepalive::kMinMs);
    ASSERT_EQ(link.calls.size(), 2U);
    link.answer_next(200, {{"nextIntervalMs", 3600000}});
    EXPECT_EQ(ka.interval_ms(), Keepalive::kMaxMs) << "acima do teto: vale o teto";
}

TEST_F(KeepaliveTest, RespostaDoBackendChegaNaAplicacao)
{
    json got;
    ka.on_reply([&](const json& p) { got = p; });
    ka.tick(g_now);
    link.answer_next(200, {{"serverTime", 5}, {"settingsResync", true}});
    EXPECT_EQ(got["settingsResync"], true);
}

TEST_F(KeepaliveTest, DuasSemRespostaReabremASessaoEOLimiteDobra)
{
    ka.tick(g_now);
    link.answer_next(504, json::object(), "sem resposta no prazo");
    EXPECT_EQ(ka.misses(), 1);
    EXPECT_EQ(link.restarts, 0);
    // Tenta de novo no piso, sem esperar o intervalo inteiro.
    ka.tick(g_now + Keepalive::kMinMs);
    ASSERT_EQ(link.calls.size(), 2U);
    link.answer_next(500);
    EXPECT_EQ(link.restarts, 1) << "o broker responde e o service não";
    EXPECT_EQ(ka.misses(), 0);

    // A segunda rodada precisa de 4.
    for (int i = 0; i < 3; ++i) {
        g_now += Keepalive::kMinMs;
        ka.tick(g_now);
        link.answer_next(504);
    }
    EXPECT_EQ(link.restarts, 1);
    g_now += Keepalive::kMinMs;
    ka.tick(g_now);
    link.answer_next(504);
    EXPECT_EQ(link.restarts, 2);
}

TEST_F(KeepaliveTest, Um4xxProvaOServiceEZeraAsFalhas)
{
    ka.tick(g_now);
    link.answer_next(504);
    EXPECT_EQ(ka.misses(), 1);
    ka.tick(g_now + Keepalive::kMinMs);
    link.answer_next(400, {{"error", "INVALID_ARGS"}}, "INVALID_ARGS");
    EXPECT_EQ(ka.misses(), 0);
    EXPECT_EQ(link.restarts, 0);
}

TEST_F(KeepaliveTest, Um503ELocalENaoContaComoFalha)
{
    ka.tick(g_now);
    link.answer_next(503, json::object(), "sessão caiu");
    EXPECT_EQ(ka.misses(), 0);
    EXPECT_FALSE(ka.in_flight());
}

TEST_F(KeepaliveTest, SemSessaoNaoMandaEReaberturaMandaNaHora)
{
    link.is_connected = false;
    ka.tick(g_now);
    EXPECT_TRUE(link.calls.empty());
    EXPECT_EQ(ka.next_in(g_now), -1);
    link.is_connected = true;
    ka.tick(g_now + 5);
    EXPECT_EQ(link.calls.size(), 1U);
    link.answer_next(200);
    // Caiu e voltou: um keepalive já na abertura, sem esperar o intervalo.
    link.is_connected = false;
    ka.tick(g_now + 10);
    link.is_connected = true;
    ka.tick(g_now + 20);
    EXPECT_EQ(link.calls.size(), 2U);
}
