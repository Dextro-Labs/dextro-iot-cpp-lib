#include <dextro-iot/procedures/remote_call.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace {

using dextro::iot::procedures::RemoteCalls;
using dextro::iot::procedures::RemoteReply;
using Json = nlohmann::json;

RemoteCalls::Request pedido(const std::string& rel, std::int64_t now_ms = 0)
{
    RemoteCalls::Request r;
    r.op = "keepalive";
    r.payload = Json{{"intervalMs", 60000}};
    r.serial = "A1B2C3D4E5F6";
    r.message_id = "m-" + rel;
    r.relation_id = rel;
    r.wall_ms = 1790000000000;
    r.now_ms = now_ms;
    return r;
}

std::string resposta(const std::string& rel, int rescode, Json payload)
{
    return Json{{"metadata", {{"relationId", rel}, {"v", "0.5"}}},
                {"op", "keepalive"},
                {"rescode", rescode},
                {"payload", std::move(payload)}}
        .dump();
}

}  // namespace

TEST(RemoteCall, EnvelopeDaSpecECasamentoPeloRelationId)
{
    RemoteCalls calls;
    std::optional<RemoteReply> got;
    const std::string body = calls.begin(pedido("r1"), [&](const RemoteReply& r) { got = r; });
    const Json env = Json::parse(body);
    EXPECT_EQ(env["op"], "keepalive");
    EXPECT_EQ(env["metadata"]["relationId"], "r1");
    EXPECT_EQ(env["metadata"]["serialNumber"], "A1B2C3D4E5F6");
    EXPECT_EQ(env["metadata"]["instance"], "A1B2C3D4E5F6");
    EXPECT_EQ(env["metadata"]["v"], "0.5");
    EXPECT_EQ(env["payload"]["intervalMs"], 60000);
    EXPECT_EQ(calls.pending(), 1U);

    // Uma resposta de outro pedido não casa.
    EXPECT_FALSE(calls.on_reply(resposta("outro", 200, Json::object())));
    EXPECT_FALSE(got);
    EXPECT_TRUE(calls.on_reply(resposta("r1", 200, Json{{"serverTime", 1790000000500}})));
    ASSERT_TRUE(got);
    EXPECT_TRUE(got->ok());
    EXPECT_EQ(got->payload["serverTime"], 1790000000500);
    EXPECT_EQ(calls.pending(), 0U);
    // Repetida (QoS 1 pode duplicar): não casa de novo.
    EXPECT_FALSE(calls.on_reply(resposta("r1", 200, Json::object())));
}

TEST(RemoteCall, RespostaDeErroTrazOCodigo)
{
    RemoteCalls calls;
    std::optional<RemoteReply> got;
    (void)calls.begin(pedido("r2"), [&](const RemoteReply& r) { got = r; });
    ASSERT_TRUE(calls.on_reply(resposta("r2", 400, Json{{"error", "INVALID_PAYLOAD"}})));
    ASSERT_TRUE(got);
    EXPECT_EQ(got->rescode, 400);
    EXPECT_EQ(got->error, "INVALID_PAYLOAD");
}

TEST(RemoteCall, PrazoVenceCom504)
{
    RemoteCalls calls;
    std::optional<RemoteReply> got;
    auto r = pedido("r3", 1000);
    r.timeout_ms = 30000;
    (void)calls.begin(r, [&](const RemoteReply& x) { got = x; });
    EXPECT_EQ(calls.next_deadline_in(1000), 30000);
    calls.expire(30999);
    EXPECT_FALSE(got);
    calls.expire(31000);
    ASSERT_TRUE(got);
    EXPECT_EQ(got->rescode, 504);
    EXPECT_EQ(calls.next_deadline_in(31000), -1);
}

TEST(RemoteCall, SessaoCaidaTerminaTodosCom503)
{
    RemoteCalls calls;
    int done = 0;
    int last = 0;
    for (const char* rel : {"a", "b", "c"}) {
        (void)calls.begin(pedido(rel), [&](const RemoteReply& x) {
            ++done;
            last = x.rescode;
        });
    }
    calls.fail_all(503, "sessão caiu");
    EXPECT_EQ(done, 3);
    EXPECT_EQ(last, 503);
    EXPECT_EQ(calls.pending(), 0U);
}

TEST(RemoteCall, FilaCheiaRecusaNaHoraCom429)
{
    RemoteCalls calls;
    for (std::size_t i = 0; i < RemoteCalls::kMaxPending; ++i) {
        EXPECT_FALSE(calls.begin(pedido("p" + std::to_string(i)), nullptr).empty());
    }
    std::optional<RemoteReply> got;
    EXPECT_TRUE(calls.begin(pedido("extra"), [&](const RemoteReply& x) { got = x; }).empty());
    ASSERT_TRUE(got);
    EXPECT_EQ(got->rescode, 429);
}

TEST(RemoteCall, LixoEmServiceReplyNaoDerruba)
{
    RemoteCalls calls;
    (void)calls.begin(pedido("r4"), nullptr);
    EXPECT_FALSE(calls.on_reply("não é json"));
    EXPECT_FALSE(calls.on_reply("[]"));
    EXPECT_FALSE(calls.on_reply(R"({"metadata":{"relationId":7}})"));
    EXPECT_EQ(calls.pending(), 1U);
}
