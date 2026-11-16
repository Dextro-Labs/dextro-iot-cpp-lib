// O despachante de eventos (`events-write`, SPEC do dextrolabs-device §4.5)
// contra um backend falso e uma fila em memória.
//
// Origem: os testes "Despachante" de tests/unit/event_manager_test.cpp do
// ssat-traffic-controller (lá contra o EventStore em SQLite e o bus de
// daemons), mais os casos que a versão assíncrona precisa: 503 sem
// penalidade, 413 dividido até isolar o item, bloco grande dividido antes de
// sair, drenagem ao conectar.
#include "fake_link.hpp"

#include <dextro-iot/event_dispatcher.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <map>

using dextro::iot::EventDispatcher;
using dextro::iot::EventStore;
using dextro::iot::StoredEvent;
using nlohmann::json;

namespace {

std::int64_t g_wall = 1790000000000;
std::int64_t fake_wall()
{
    return g_wall;
}

/// A fila da aplicação, em memória, com a semântica do port.
class MemoryStore final : public EventStore {
public:
    class Row {
    public:
        StoredEvent event;
        std::int64_t next_touch = 0;
        bool rejected = false;
        std::string why;
    };
    std::map<std::int64_t, Row> rows;
    std::int64_t next_seq = 1;
    int reschedules = 0;

    std::int64_t append(std::string code, std::string args, std::int64_t occurred_ms)
    {
        Row r;
        r.event.seq = next_seq++;
        r.event.code = std::move(code);
        r.event.args = std::move(args);
        r.event.occurred_ms = occurred_ms;
        rows[r.event.seq] = r;
        return r.event.seq;
    }
    std::size_t pending() const
    {
        return static_cast<std::size_t>(std::count_if(
            rows.begin(), rows.end(), [](const auto& kv) { return !kv.second.rejected; }));
    }

    std::vector<StoredEvent> due(std::int64_t now, std::size_t limit) override
    {
        std::vector<StoredEvent> out;
        for (const auto& [seq, r] : rows) {
            (void)seq;
            if (!r.rejected && r.next_touch <= now && out.size() < limit) {
                out.push_back(r.event);
            }
        }
        return out;
    }
    std::size_t acknowledge(std::int64_t ack, const std::vector<std::int64_t>& seqs) override
    {
        std::size_t removed = 0;
        for (auto it = rows.begin(); it != rows.end();) {
            const bool listed = std::find(seqs.begin(), seqs.end(), it->first) != seqs.end();
            if (!it->second.rejected && (it->first <= ack || listed)) {
                it = rows.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }
    void reject(std::int64_t seq, std::string_view reason) override
    {
        auto it = rows.find(seq);
        if (it != rows.end()) {
            it->second.rejected = true;
            it->second.why = std::string(reason);
        }
    }
    void reschedule(const std::vector<std::int64_t>& seqs, std::int64_t next, std::string_view why,
                    bool count_attempt) override
    {
        ++reschedules;
        for (std::int64_t seq : seqs) {
            auto it = rows.find(seq);
            if (it != rows.end()) {
                it->second.next_touch = next;
                it->second.why = std::string(why);
                if (count_attempt) {
                    ++it->second.event.attempts;
                }
            }
        }
    }
    std::optional<std::int64_t> next_touch() override
    {
        std::optional<std::int64_t> best;
        for (const auto& [seq, r] : rows) {
            (void)seq;
            if (!r.rejected && (!best || r.next_touch < *best)) {
                best = r.next_touch;
            }
        }
        return best;
    }
};

/// O backend que aceita tudo: `ackSeq` = o maior seq do bloco.
dextro::iot::RemoteResult ok_all(const FakeLink::Call& c)
{
    std::int64_t max = 0;
    for (const json& e : c.payload["events"]) {
        max = std::max<std::int64_t>(max, e["seq"].get<std::int64_t>());
    }
    return FakeLink::result(200, {{"accepted", c.payload["events"].size()}, {"ackSeq", max}});
}

class Dispatcher : public ::testing::Test {
protected:
    /// Algumas voltas do laço, como a thread da aplicação faria.
    void spin(int rounds = 20)
    {
        for (int i = 0; i < rounds; ++i) {
            now += 10;
            d.tick(now);
        }
    }

    std::int64_t now = 1000;
    FakeLink link;
    MemoryStore store;
    EventDispatcher d{link, store, 60000, &fake_wall};
};

}  // namespace

TEST_F(Dispatcher, BlocoAceitoSaiDaFilaComOccurredAtOriginal)
{
    store.append("power-up", R"({"restartCause":"first-boot"})", 1790000000123);
    link.auto_answer = ok_all;
    spin();
    EXPECT_EQ(store.pending(), 0U);
    ASSERT_GE(link.calls.size(), 1U);
    const auto& c = link.calls.front();
    EXPECT_EQ(c.op, "events-write");
    EXPECT_EQ(c.timeout_ms, EventDispatcher::kCallTimeoutMs);
    const json& ev = c.payload["events"][0];
    EXPECT_EQ(ev["seq"], 1);
    EXPECT_EQ(ev["code"], "power-up");
    EXPECT_EQ(ev["occurredAt"], 1790000000123);
    EXPECT_EQ(ev["args"]["restartCause"], "first-boot");
    EXPECT_EQ(d.sent(), 1U);
}

TEST_F(Dispatcher, AckSeqDescartaTudoAteEleMesmoForaDoBloco)
{
    // §4.5: o device PODE descartar tudo que for <= ackSeq.
    for (int i = 0; i < 3; ++i) {
        store.append("vpn-up", "{}", 1000 + i);
    }
    link.auto_answer = [](const FakeLink::Call&) {
        return FakeLink::result(200, {{"accepted", 0}, {"ackSeq", 3}});
    };
    spin();
    EXPECT_EQ(store.pending(), 0U);
}

TEST_F(Dispatcher, RecusadoDeVezNaoVoltaEClockSkewVolta)
{
    for (int i = 0; i < 3; ++i) {
        store.append("vpn-up", "{}", 1000 + i);
    }
    link.auto_answer = [](const FakeLink::Call&) {
        return FakeLink::result(
            200, {{"accepted", 1},
                  {"ackSeq", 1},
                  {"rejected", json::array({json{{"seq", 2}, {"reason", "INVALID_ARGS"}},
                                            json{{"seq", 3}, {"reason", "CLOCK_SKEW"}}})}});
    };
    spin();
    // Sobrou só o 3, reagendado para depois (CLOCK_SKEW), com tentativa contada.
    EXPECT_EQ(store.pending(), 1U);
    EXPECT_TRUE(store.rows.at(2).rejected);
    EXPECT_TRUE(store.due(g_wall, 10).empty());
    const auto later = store.due(g_wall + EventDispatcher::kBackoffCapMs, 10);
    ASSERT_EQ(later.size(), 1U);
    EXPECT_EQ(later[0].seq, 3);
    EXPECT_EQ(later[0].attempts, 1);
}

TEST_F(Dispatcher, Bloco413EDivididoAoMeioAteIsolarOItem)
{
    for (int i = 0; i < 4; ++i) {
        store.append(i == 2 ? "grande" : "vpn-up", "{}", 1000 + i);
    }
    // O backend recusa qualquer bloco que tenha o item "grande".
    link.auto_answer = [](const FakeLink::Call& c) {
        for (const json& e : c.payload["events"]) {
            if (e["code"] == "grande") {
                return FakeLink::result(413, {{"error", "PAYLOAD_TOO_LARGE"}}, "PAYLOAD_TOO_LARGE");
            }
        }
        return ok_all(c);
    };
    spin();
    // O "grande" virou recusado (fora da fila); os outros três foram aceitos.
    EXPECT_EQ(store.pending(), 0U);
    EXPECT_TRUE(store.rows.at(3).rejected);
    EXPECT_EQ(d.sent(), 3U);
    EXPECT_EQ(d.rejected(), 1U);
    // 4 → [1,2] [3,4] → [3] [4]: o bloco inteiro, as duas metades, e as duas
    // de um da metade ruim.
    ASSERT_EQ(link.calls.size(), 5U);
    EXPECT_EQ(link.calls[0].payload["events"].size(), 4U);
    EXPECT_EQ(link.calls[1].payload["events"].size(), 2U);
    EXPECT_EQ(link.calls[2].payload["events"].size(), 2U);
    EXPECT_EQ(link.calls[3].payload["events"].size(), 1U);
    EXPECT_EQ(link.calls[4].payload["events"].size(), 1U);
    EXPECT_EQ(store.reschedules, 0) << "413 não é motivo de backoff: é divisão";
}

TEST_F(Dispatcher, Bloco400TambemEDividido)
{
    store.append("vpn-up", "{}", 1000);
    store.append("torto", "{}", 1001);
    link.auto_answer = [](const FakeLink::Call& c) {
        for (const json& e : c.payload["events"]) {
            if (e["code"] == "torto") {
                return FakeLink::result(400, {{"error", "INVALID_ARGS"}}, "INVALID_ARGS");
            }
        }
        return ok_all(c);
    };
    spin();
    EXPECT_EQ(store.pending(), 0U);
    EXPECT_TRUE(store.rows.at(2).rejected);
    EXPECT_EQ(d.sent(), 1U);
}

TEST_F(Dispatcher, Um503NaoTemPenalidade)
{
    store.append("vpn-up", "{}", 1000);
    store.append("vpn-down", "{}", 1001);
    link.auto_answer = [](const FakeLink::Call&) {
        return FakeLink::result(503, json::object(), "sem sessão");
    };
    spin();
    ASSERT_EQ(link.calls.size(), 1U) << "503 encerra a drenagem: não insiste";
    // Nada reagendado, nenhuma tentativa contada, nada recusado: o evento
    // continua devido, e a próxima sessão drena.
    EXPECT_EQ(store.reschedules, 0);
    EXPECT_EQ(store.pending(), 2U);
    EXPECT_EQ(store.due(g_wall, 10).size(), 2U);
    for (const auto& [seq, r] : store.rows) {
        EXPECT_EQ(r.event.attempts, 0) << seq;
    }
    // A sessão cai e volta: drena na hora, sem esperar o intervalo.
    link.auto_answer = ok_all;
    link.is_connected = false;
    spin(1);
    link.is_connected = true;
    spin(2);
    EXPECT_EQ(store.pending(), 0U);
    EXPECT_EQ(link.calls.size(), 2U);
}

TEST_F(Dispatcher, Um503ComOPedidoEmVooTambemNaoPenaliza)
{
    store.append("vpn-up", "{}", 1000);
    spin(1);
    ASSERT_EQ(link.calls.size(), 1U);
    link.answer_next(503, json::object(), "sessão caiu");
    EXPECT_FALSE(d.in_flight());
    EXPECT_EQ(store.reschedules, 0);
    EXPECT_EQ(store.due(g_wall, 10).size(), 1U);
}

TEST_F(Dispatcher, SilencioDoBackendReagendaComBackoff)
{
    store.append("vpn-up", "{}", 1000);
    link.auto_answer = [](const FakeLink::Call&) {
        return FakeLink::result(504, json::object(), "sem resposta no prazo");
    };
    spin();
    EXPECT_EQ(link.calls.size(), 1U);
    EXPECT_EQ(store.pending(), 1U);
    EXPECT_TRUE(store.due(g_wall, 10).empty());  // reagendado para o futuro
    const std::int64_t at = store.rows.at(1).next_touch - g_wall;
    EXPECT_GE(at, EventDispatcher::kBackoffFirstMs / 2);
    EXPECT_LE(at, EventDispatcher::kBackoffFirstMs);
    EXPECT_EQ(store.rows.at(1).event.attempts, 1);
}

TEST_F(Dispatcher, BlocoNuncaPassaDe200Eventos)
{
    for (int i = 0; i < 450; ++i) {
        store.append("tick", "{}", 1000 + i);
    }
    link.auto_answer = ok_all;
    spin();
    EXPECT_EQ(store.pending(), 0U);
    ASSERT_EQ(link.calls.size(), 3U);
    EXPECT_EQ(link.calls[0].payload["events"].size(), EventDispatcher::kMaxBatch);
    EXPECT_EQ(link.calls[1].payload["events"].size(), EventDispatcher::kMaxBatch);
    EXPECT_EQ(link.calls[2].payload["events"].size(), 50U);
}

TEST_F(Dispatcher, BlocoAcimaDe256KBEDivididoAntesDeSair)
{
    // Quatro eventos de ~100 KB: o bloco inteiro passaria dos 256 KB.
    const std::string big = R"({"blob":")" + std::string(100 * 1024, 'x') + R"("})";
    for (int i = 0; i < 4; ++i) {
        store.append("big", big, 1000 + i);
    }
    // E um sozinho acima do limite: recusado aqui, nunca vai ao backend.
    store.append("huge", R"({"blob":")" + std::string(300 * 1024, 'y') + R"("})", 2000);
    link.auto_answer = ok_all;
    spin(40);
    for (const auto& c : link.calls) {
        EXPECT_LE(c.payload.dump().size(), EventDispatcher::kMaxBatchBytes);
        for (const json& e : c.payload["events"]) {
            EXPECT_NE(e["code"], "huge");
        }
    }
    EXPECT_EQ(d.sent(), 4U);
    EXPECT_TRUE(store.rows.at(5).rejected);
    EXPECT_EQ(store.pending(), 0U);
}

TEST_F(Dispatcher, SemSessaoNaoTentaEKickAcordaADrenagem)
{
    link.is_connected = false;
    store.append("vpn-up", "{}", 1000);
    spin();
    EXPECT_TRUE(link.calls.empty());

    link.is_connected = true;
    link.auto_answer = ok_all;
    spin(1);  // conectou: drena
    EXPECT_EQ(link.calls.size(), 1U);
    spin(1);  // a volta seguinte acha a fila vazia e dorme
    // Fila vazia: dorme até o intervalo... a menos que um evento novo chegue.
    store.append("vpn-down", "{}", 1001);
    spin(1);
    EXPECT_EQ(link.calls.size(), 1U);
    d.kick();
    spin(1);
    EXPECT_EQ(link.calls.size(), 2U);
    EXPECT_EQ(store.pending(), 0U);
}
