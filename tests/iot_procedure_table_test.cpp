// Origem: tests/unit/iot_procedure_table_test.cpp do ssat-traffic-controller.
// O enum ProcedureId do controlador virou registro: aqui a tabela é montada
// como uma aplicação a montaria — as ops genéricas da biblioteca e duas ops
// "do tipo" sintéticas — e os mesmos contratos (404, 403, 413, idempotencyKey,
// outbox_only) são conferidos contra ela.
#include <dextro-iot/procedures/ops/certificate_install.hpp>
#include <dextro-iot/procedures/ops/certificate_request.hpp>
#include <dextro-iot/procedures/ops/get_status.hpp>
#include <dextro-iot/procedures/ops/list_procedures.hpp>
#include <dextro-iot/procedures/ops/outbox_notification.hpp>
#include <dextro-iot/procedures/ops/settings_read.hpp>
#include <dextro-iot/procedures/ops/settings_write.hpp>
#include <dextro-iot/procedures/procedure_table.hpp>

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <set>
#include <string>

using dextro::iot::procedures::Json;
using dextro::iot::procedures::Procedure;
using dextro::iot::procedures::ProcedureSpec;
using dextro::iot::procedures::ProcedureTable;
using dextro::iot::procedures::Reply;
using dextro::iot::procedures::Request;
using dextro::iot::procedures::Rescode;

namespace ops = dextro::iot::procedures;

namespace {

// As oito que todo device tem que atender (SPEC do dextrolabs-device §4.6)
// menos o `ota-notification`, que é da aplicação (formatos e agente de OTA de
// cada produto).
const std::set<std::string> kGeneric = {
    "list-procedures", "get-status",          "outbox-notification", "settings-read",
    "settings-write",  "certificate-request", "certificate-install",
};

/// Uma op "do tipo", como a aplicação escreveria.
class Echo final : public Procedure {
public:
    std::optional<Reply> validate(const Json& payload) const override
    {
        return ops::only_keys(payload, {"x"});
    }
    Reply execute(const Request& request) override { return Reply::ok(request.payload); }
};

void register_all(ProcedureTable& table)
{
    ASSERT_TRUE(table.add({"list-procedures", 1, false, 30000, false},
                          std::make_unique<ops::ListProcedures>(table, "allprint-test", 3)));
    ASSERT_TRUE(table.add({"get-status", 1, false, 30000, false},
                          std::make_unique<ops::GetStatus>(nullptr)));
    ASSERT_TRUE(table.add({"outbox-notification", 1, false, 10000, false},
                          std::make_unique<ops::OutboxNotification>(nullptr)));
    ASSERT_TRUE(table.add({"settings-read", 1, false, 30000, false},
                          std::make_unique<ops::SettingsRead>(nullptr)));
    ASSERT_TRUE(table.add({"settings-write", 1, false, 60000, false},
                          std::make_unique<ops::SettingsWrite>(nullptr, nullptr)));
    ASSERT_TRUE(table.add({"certificate-request", 1, true, 120000, false},
                          std::make_unique<ops::CertificateRequest>(nullptr)));
    ASSERT_TRUE(table.add({"certificate-install", 1, true, 60000, false},
                          std::make_unique<ops::CertificateInstall>(nullptr, nullptr)));
    // Do tipo: um comando v2 e uma escrita de configuração só pela outbox.
    ASSERT_TRUE(table.add({"door-command", 2, false, 15000, false}, std::make_unique<Echo>()));
    ASSERT_TRUE(table.add({"recipe-put", 1, false, 60000, true}, std::make_unique<Echo>()));
}

class Built : public ::testing::Test {
protected:
    void SetUp() override { register_all(table); }
    ProcedureTable table;
};

}  // namespace

TEST(IotProcedureTable, RegistroRecusaNomeVazioRepetidoEClasseNula)
{
    ProcedureTable table;
    EXPECT_FALSE(table.add({"", 1, false, 1000, false}, std::make_unique<Echo>()));
    EXPECT_FALSE(table.add({"door-command", 1, false, 1000, false}, nullptr));
    EXPECT_TRUE(table.add({"door-command", 1, false, 1000, false}, std::make_unique<Echo>()));
    EXPECT_FALSE(table.add({"door-command", 2, false, 1000, false}, std::make_unique<Echo>()))
        << "o mesmo nome duas vezes: qual das duas atende?";
    EXPECT_EQ(table.size(), 1U);
    EXPECT_EQ(table.spec("door-command")->version, 1);
}

TEST_F(Built, NomeDesconhecidoNaoVira)
{
    EXPECT_EQ(table.at("device-status"), nullptr);
    EXPECT_EQ(table.at(""), nullptr);
    EXPECT_EQ(table.at("GET-STATUS"), nullptr);
}

TEST_F(Built, SoAsDeCertificadoValemNoBootstrap)
{
    table.for_each([](const ProcedureSpec& line) {
        const bool certificate =
            line.op == "certificate-request" || line.op == "certificate-install";
        EXPECT_EQ(line.bootstrap, certificate) << line.op;
    });
}

TEST_F(Built, ListProceduresDevolveAsGenericasEAsDoTipoComVersao)
{
    const auto reply = table.dispatch("list-procedures", Request{}, false);
    ASSERT_EQ(reply.rescode, Rescode::Ok);
    EXPECT_EQ(reply.payload["deviceType"], "allprint-test");
    EXPECT_EQ(reply.payload["contractVersion"], 3);

    std::set<std::string> listed;
    std::map<std::string, int> version;
    for (const Json& item : reply.payload["procedures"]) {
        listed.insert(item["op"].get<std::string>());
        version[item["op"].get<std::string>()] = item["version"].get<int>();
        EXPECT_GE(item["version"].get<int>(), 1);
    }
    std::set<std::string> expected = kGeneric;
    expected.insert({"door-command", "recipe-put"});
    EXPECT_EQ(listed, expected);
    // É a versão que o backend usa para escolher o schema (A.4.1).
    EXPECT_EQ(version["door-command"], 2);
}

TEST_F(Built, OpDesconhecidaE404)
{
    const auto reply = table.dispatch("reboot", Request{}, false);
    EXPECT_EQ(reply.rescode, Rescode::NotFound);
    EXPECT_EQ(reply.payload["error"], "UNKNOWN_OP");
    EXPECT_EQ(reply.payload["detail"], "reboot");
}

TEST_F(Built, EscopoBootstrapRecusaOpNaoDeCertificado)
{
    const auto reply = table.dispatch("get-status", Request{}, /*bootstrap_scope=*/true);
    EXPECT_EQ(reply.rescode, Rescode::Forbidden);
    EXPECT_EQ(reply.payload["error"], "OUT_OF_SCOPE");
}

TEST_F(Built, EscopoBootstrapDeixaPassarCertificado)
{
    // Sem store de certificado: o que importa é que NÃO é 403.
    const auto reply = table.dispatch_text(
        "certificate-request",
        R"({"requestId":"0190f7a2-6c1e-7b3a-9f00-1234567890ab","reason":"initial","keyType":"EC-P256"})",
        "rel", /*bootstrap_scope=*/true);
    EXPECT_NE(reply.rescode, Rescode::Forbidden);
    EXPECT_EQ(reply.payload["error"], "UNAVAILABLE");
}

TEST_F(Built, PayloadIlegivelE400)
{
    const auto reply = table.dispatch_text("list-procedures", "{nao e json", "rel", false);
    EXPECT_EQ(reply.rescode, Rescode::BadRequest);
    EXPECT_EQ(reply.payload["error"], "INVALID_ARGS");
}

TEST_F(Built, PayloadQueNaoEObjetoE400)
{
    EXPECT_EQ(table.dispatch_text("list-procedures", "[1,2]", "rel", false).rescode,
              Rescode::BadRequest);
}

TEST_F(Built, PayloadVazioValeObjetoVazio)
{
    EXPECT_EQ(table.dispatch_text("list-procedures", "", "rel", false).rescode, Rescode::Ok);
}

TEST_F(Built, PayloadAcimaDoLimiteE413)
{
    const std::string big(ProcedureTable::kMaxPayload + 1, ' ');
    EXPECT_EQ(table.dispatch_text("list-procedures", big, "rel", false).rescode,
              Rescode::PayloadTooLarge);
}

TEST_F(Built, IdempotencyKeyEDoProtocoloNaoDaOp)
{
    // `list-procedures` tem `maxProperties: 0`. Com a chave ainda no payload
    // seria 400; ela tem que sair antes de chegar à op.
    const auto reply =
        table.dispatch_text("list-procedures", R"({"idempotencyKey":"k-1"})", "rel", false);
    EXPECT_EQ(reply.rescode, Rescode::Ok);

    EXPECT_EQ(table.dispatch_text("list-procedures", R"({"idempotencyKey":7})", "rel", false).rescode,
              Rescode::BadRequest);
}

TEST_F(Built, CampoForaDoSchemaE400)
{
    const auto reply = table.dispatch_text("list-procedures", R"({"x":1})", "rel", false);
    EXPECT_EQ(reply.rescode, Rescode::BadRequest);
    EXPECT_EQ(reply.payload["error"], "INVALID_ARGS");
}

TEST_F(Built, OpDoTipoRegistradaPelaAplicacaoAtende)
{
    const auto reply = table.dispatch_payload("door-command", Json{{"x", 7}}, "rel", false);
    EXPECT_EQ(reply.rescode, Rescode::Ok);
    EXPECT_EQ(reply.payload["x"], 7);
}

// --- outbox_only: escrita de configuração só pela outbox durável --------------

TEST_F(Built, OutboxOnlyPelaRpcE403EPelaOutboxPassa)
{
    const auto rpc = table.dispatch_payload("recipe-put", Json{{"x", 1}}, "rel", false);
    EXPECT_EQ(rpc.rescode, Rescode::Forbidden);
    EXPECT_EQ(rpc.payload["error"], "OUTBOX_ONLY");
    const auto outbox = table.dispatch_payload("recipe-put", Json{{"x", 1}}, "rel", false,
                                               ops::Origin::Outbox);
    EXPECT_EQ(outbox.rescode, Rescode::Ok);
    const auto local = table.dispatch_payload("recipe-put", Json{{"x", 1}}, "rel", false,
                                              ops::Origin::Local);
    EXPECT_EQ(local.rescode, Rescode::Ok);
}

TEST(IotProcedureTable, OpOutboxOnlyRecusaRpcCom403EAceitaOutbox)
{
    using dextro::iot::procedures::admit;
    using dextro::iot::procedures::Origin;
    // Uma linha sintética: a porta é a mesma que o `dispatch` usa.
    const ProcedureSpec row{"plans-put", 1, false, 60000, /*outbox_only=*/true};
    const auto refused = admit(row, /*bootstrap_scope=*/false, Origin::Rpc);
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->rescode, Rescode::Forbidden);
    EXPECT_EQ(refused->payload["error"], "OUTBOX_ONLY");
    EXPECT_EQ(refused->payload["detail"], "plans-put");

    EXPECT_FALSE(admit(row, false, Origin::Outbox).has_value());
    // O escopo bootstrap continua valendo antes da origem.
    const auto boot = admit(row, true, Origin::Outbox);
    ASSERT_TRUE(boot.has_value());
    EXPECT_EQ(boot->payload["error"], "OUT_OF_SCOPE");

    // Linha comum: as duas origens passam.
    const ProcedureSpec plain{"get-status", 1, false, 30000, false};
    EXPECT_FALSE(admit(plain, false, Origin::Rpc).has_value());
    EXPECT_FALSE(admit(plain, false, Origin::Outbox).has_value());
}

TEST_F(Built, OrigemOutboxDespachaPelaMesmaTabela)
{
    const auto reply = table.dispatch("list-procedures", Request{}, false,
                                      dextro::iot::procedures::Origin::Outbox);
    EXPECT_EQ(reply.rescode, Rescode::Ok);
}
