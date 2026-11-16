#include <dextro-iot/procedures/envelope.hpp>

#include <gtest/gtest.h>

using namespace dextro::iot::procedures;

namespace {

const std::string kSerial = "A1B2C3D4E5F6";

std::string request(const std::string& meta_extra, const std::string& rest)
{
    return R"({"metadata":{"v":"0.5","messageId":"m-1","relationId":"r-1","timestamp":1,)"
           R"("instance":"svc-iot-devices-0")" +
           meta_extra + "}" + rest + "}";
}

}  // namespace

TEST(IotEnvelope, RequestValidoDecodifica)
{
    const Parsed p =
        parse_request(request(R"(,"serialNumber":"A1B2C3D4E5F6")", R"(,"op":"get-status","payload":{"sections":["link"]})"),
                      kSerial);
    ASSERT_TRUE(p.replyable);
    EXPECT_FALSE(p.refusal.has_value());
    EXPECT_EQ(p.envelope.op, "get-status");
    EXPECT_EQ(p.envelope.relation_id, "r-1");
    EXPECT_EQ(p.envelope.instance, "svc-iot-devices-0");
    EXPECT_EQ(p.envelope.payload["sections"][0], "link");
}

TEST(IotEnvelope, SemPayloadValeObjetoVazio)
{
    const Parsed p = parse_request(request("", R"(,"op":"list-procedures")"), kSerial);
    ASSERT_TRUE(p.replyable);
    EXPECT_TRUE(p.envelope.payload.is_object());
}

TEST(IotEnvelope, SemInstanceNaoHaAQuemResponder)
{
    const Parsed p = parse_request(R"({"metadata":{"v":"0.5","relationId":"r"},"op":"get-status"})", kSerial);
    EXPECT_FALSE(p.replyable);
    EXPECT_FALSE(p.problem.empty());
}

TEST(IotEnvelope, InstanceComCuringaNaoViraTopico)
{
    for (const char* bad : {"pod/x", "pod+", "#"}) {
        const std::string text = std::string(R"({"metadata":{"v":"0.5","relationId":"r","instance":")") +
                                 bad + R"("},"op":"get-status"})";
        EXPECT_FALSE(parse_request(text, kSerial).replyable) << bad;
    }
}

TEST(IotEnvelope, JsonIlegivelNaoEReplyable)
{
    EXPECT_FALSE(parse_request("{nao", kSerial).replyable);
    EXPECT_FALSE(parse_request("[]", kSerial).replyable);
}

TEST(IotEnvelope, VersaoMaiorDiferenteE400)
{
    std::string text = request("", R"(,"op":"get-status")");
    text.replace(text.find("0.5"), 3, "1.0");
    const Parsed p = parse_request(text, kSerial);
    ASSERT_TRUE(p.replyable);
    ASSERT_TRUE(p.refusal.has_value());
    EXPECT_EQ(p.refusal->rescode, Rescode::BadRequest);
}

TEST(IotEnvelope, VersaoMenorDiferenteEAceita)
{
    std::string text = request("", R"(,"op":"get-status")");
    text.replace(text.find("0.5"), 3, "0.4");
    EXPECT_FALSE(parse_request(text, kSerial).refusal.has_value());
}

TEST(IotEnvelope, SerialDeOutroDeviceE403)
{
    const Parsed p =
        parse_request(request(R"(,"serialNumber":"FFFFFFFFFFFF")", R"(,"op":"get-status")"), kSerial);
    ASSERT_TRUE(p.refusal.has_value());
    EXPECT_EQ(p.refusal->rescode, Rescode::Forbidden);
}

TEST(IotEnvelope, RespostaEcoaORelationIdELevaORescode)
{
    Envelope e;
    e.relation_id = "r-9";
    e.instance = "svc-1";
    e.op = "get-status";
    const std::string text = build_reply(e, Reply::accepted(), kSerial, "m-new", 42);
    const Json j = Json::parse(text);
    EXPECT_EQ(j["metadata"]["relationId"], "r-9");
    EXPECT_EQ(j["metadata"]["instance"], "svc-1");
    EXPECT_EQ(j["metadata"]["messageId"], "m-new");
    EXPECT_EQ(j["metadata"]["serialNumber"], kSerial);
    EXPECT_EQ(j["metadata"]["timestamp"], 42);
    EXPECT_EQ(j["op"], "get-status");
    EXPECT_EQ(j["rescode"], 202);
    EXPECT_EQ(reply_topic(kSerial, "svc-1"), "atrio/A1B2C3D4E5F6/procedure/reply/svc-1");
}

// --- TTL (spec §8) -------------------------------------------------------------

namespace {

std::string at(std::int64_t timestamp)
{
    return R"({"metadata":{"v":"0.5","relationId":"r","instance":"svc","timestamp":)" +
           std::to_string(timestamp) + R"(},"op":"get-status"})";
}

Clock synced(std::int64_t now)
{
    Clock c;
    c.wall_ms = now;
    c.synchronized = true;
    return c;
}

constexpr std::int64_t kNow = 1790000000000;

}  // namespace

TEST(IotEnvelopeTtl, PedidoRecenteVale)
{
    EXPECT_FALSE(parse_request(at(kNow - 2000), kSerial, synced(kNow)).refusal.has_value());
}

TEST(IotEnvelopeTtl, PedidoVelhoE504ComResposta)
{
    const Parsed p = parse_request(at(kNow - kRequestTtlMs - 1000), kSerial, synced(kNow));
    ASSERT_TRUE(p.replyable) << "TTL expirado responde; nunca descarta em silêncio";
    ASSERT_TRUE(p.refusal.has_value());
    EXPECT_EQ(p.refusal->rescode, Rescode::Timeout);
    EXPECT_EQ(p.refusal->payload["error"], "TTL_EXPIRED");
}

TEST(IotEnvelopeTtl, PedidoDoFuturoAlemDaToleranciaEClockSkew)
{
    const Parsed p = parse_request(at(kNow + kClockSkewMs + 1000), kSerial, synced(kNow));
    ASSERT_TRUE(p.refusal.has_value());
    EXPECT_EQ(p.refusal->payload["error"], "CLOCK_SKEW");
    EXPECT_FALSE(parse_request(at(kNow + 3000), kSerial, synced(kNow)).refusal.has_value());
}

TEST(IotEnvelopeTtl, TimestampEmSegundosENormalizado)
{
    EXPECT_FALSE(parse_request(at(kNow / 1000 - 2), kSerial, synced(kNow)).refusal.has_value());
    EXPECT_TRUE(parse_request(at(kNow / 1000 - 120), kSerial, synced(kNow)).refusal.has_value());
}

TEST(IotEnvelopeTtl, RelogioSemSincroniaDesligaOTtl)
{
    Clock unsynced;
    unsynced.wall_ms = 0;  // 1970: o controlador acabou de ligar, antes do GNSS
    EXPECT_FALSE(parse_request(at(kNow), kSerial, unsynced).refusal.has_value());
    EXPECT_FALSE(parse_request(at(1), kSerial, unsynced).refusal.has_value());
}

TEST(IotEnvelopeTtl, SemTimestampNaoRecusa)
{
    const std::string text = R"({"metadata":{"v":"0.5","relationId":"r","instance":"svc"},"op":"get-status"})";
    EXPECT_FALSE(parse_request(text, kSerial, synced(kNow)).refusal.has_value());
}
