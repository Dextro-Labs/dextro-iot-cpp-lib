// Origem: tests/unit/iot_procedures_test.cpp do ssat-traffic-controller, só
// com as ops genéricas. Os testes de ota-notification, ring-mode, plan-command
// e test-seed-install ficaram lá, com as ops.
#include <dextro-iot/procedures/ops/certificate_install.hpp>
#include <dextro-iot/procedures/ops/certificate_request.hpp>
#include <dextro-iot/procedures/ops/get_status.hpp>
#include <dextro-iot/procedures/ops/list_procedures.hpp>
#include <dextro-iot/procedures/ops/outbox_notification.hpp>
#include <dextro-iot/procedures/ops/settings_read.hpp>
#include <dextro-iot/procedures/ops/settings_write.hpp>
#include <dextro-iot/procedures/procedure_table.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <tuple>

#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dextro::iot::procedures;

namespace {

// --- os ports de mentira ----------------------------------------------------

class FakeStatus final : public StatusSource {
public:
    std::vector<std::string> sections() const override { return {"system", "link"}; }
    bool fill(std::string_view section, Json& out) const override
    {
        if (section == "system") {
            out = {{"version", "0.1.0"}};
            return true;
        }
        if (section == "link") {
            out = {{"up", true}};
            return true;
        }
        return false;
    }
};

class FakeSettings final : public SettingsStore {
public:
    std::int64_t applied = 3;
    bool persist = true;
    bool affects_session = false;
    std::vector<Rejection> refuse;
    Json received;
    int extra = 0;  ///< itens a mais na coleção, para a paginação

    std::int64_t version() const override { return applied; }
    Json read(const std::vector<std::string>& refs) const override
    {
        Json out = Json::array();
        out.push_back({{"name", "broker.url"}, {"filtered", !refs.empty()}});
        for (int i = 0; i < extra; ++i) {
            out.push_back({{"name", "item." + std::to_string(i)}});
        }
        return out;
    }
    bool apply(std::int64_t version, const Json& settings, std::vector<Rejection>& rejected,
               bool& session_affected) override
    {
        received = settings;
        if (!persist) {
            return false;
        }
        applied = version;
        rejected = refuse;
        session_affected = affects_session;
        return true;
    }
};

class FakeCertificates final : public CertificateStore {
public:
    Result next = Result::Ok;
    std::string last_request;

    Result request_csr(std::string_view request_id, std::string& csr_pem) override
    {
        last_request = std::string(request_id);
        csr_pem = "-----BEGIN CERTIFICATE REQUEST-----\nAAAA\n-----END CERTIFICATE REQUEST-----\n";
        return next;
    }
    Result install(std::string_view, std::string_view, std::string& fingerprint) override
    {
        fingerprint = std::string(64, 'a');
        return next;
    }
};

class FakeSession final : public SessionControl {
public:
    int drains = 0;
    int reconnects = 0;
    void request_outbox_drain() override { ++drains; }
    void request_reconnect() override { ++reconnects; }
    bool bootstrap_scope() const override { return false; }
};

/// A tabela como a aplicação a montaria com as ops genéricas. Ports nulos
/// valem: a op responde 500 UNAVAILABLE.
void register_generic(ProcedureTable& table, const StatusSource* status, SettingsStore* settings,
                      CertificateStore* certificates, SessionControl* session)
{
    table.add({"list-procedures", 1, false, 30000, false},
              std::make_unique<ListProcedures>(table, "test", 1));
    table.add({"get-status", 1, false, 30000, false}, std::make_unique<GetStatus>(status));
    table.add({"outbox-notification", 1, false, 10000, false},
              std::make_unique<OutboxNotification>(session));
    table.add({"settings-read", 1, false, 30000, false}, std::make_unique<SettingsRead>(settings));
    table.add({"settings-write", 1, false, 60000, false},
              std::make_unique<SettingsWrite>(settings, session));
    table.add({"certificate-request", 1, true, 120000, false},
              std::make_unique<CertificateRequest>(certificates));
    table.add({"certificate-install", 1, true, 60000, false},
              std::make_unique<CertificateInstall>(certificates, session));
}

class Procedures : public ::testing::Test {
protected:
    void SetUp() override
    {
        register_generic(table, &status, &settings, &certificates, &session);
    }

    Reply call(std::string_view op, Json payload)
    {
        Request request;
        request.relation_id = "rel-1";
        request.payload = std::move(payload);
        return table.dispatch(op, request, false);
    }

    FakeStatus status;
    FakeSettings settings;
    FakeCertificates certificates;
    FakeSession session;
    ProcedureTable table;
};

const std::string kUuid = "0190f7a2-6c1e-7b3a-9f00-1234567890ab";

}  // namespace

// --- get-status ---------------------------------------------------------------

TEST_F(Procedures, GetStatusSemSecoesDevolveTodas)
{
    const auto reply = call("get-status", Json::object());
    ASSERT_EQ(reply.rescode, Rescode::Ok);
    EXPECT_EQ(reply.payload["system"]["version"], "0.1.0");
    EXPECT_EQ(reply.payload["link"]["up"], true);
}

TEST_F(Procedures, GetStatusAllEOMesmoQueTodas)
{
    const auto reply = call("get-status", {{"sections", {"all"}}});
    ASSERT_EQ(reply.rescode, Rescode::Ok);
    EXPECT_TRUE(reply.payload.contains("system"));
    EXPECT_TRUE(reply.payload.contains("link"));
}

TEST_F(Procedures, GetStatusFiltraPorSecao)
{
    const auto reply = call("get-status", {{"sections", {"link"}}});
    ASSERT_EQ(reply.rescode, Rescode::Ok);
    EXPECT_FALSE(reply.payload.contains("system"));
    EXPECT_TRUE(reply.payload.contains("link"));
}

TEST_F(Procedures, GetStatusSecaoDesconhecidaE400)
{
    EXPECT_EQ(call("get-status", {{"sections", {"enlace"}}}).rescode, Rescode::BadRequest);
}

TEST_F(Procedures, GetStatusSecaoRepetidaE400)
{
    EXPECT_EQ(call("get-status", {{"sections", {"link", "link"}}}).rescode, Rescode::BadRequest);
}

// --- ota-notification ---------------------------------------------------------

TEST_F(Procedures, OutboxNotificationPedeDrenagemE204)
{
    const auto reply = call("outbox-notification", {{"entities", {"plan"}}, {"size", 2}});
    EXPECT_EQ(reply.rescode, Rescode::NoContent);
    EXPECT_EQ(session.drains, 1);
}

TEST_F(Procedures, OutboxNotificationSemSizeE400)
{
    EXPECT_EQ(call("outbox-notification", {{"entities", Json::array()}}).rescode,
              Rescode::BadRequest);
    EXPECT_EQ(session.drains, 0);
}

// --- settings-read / settings-write ------------------------------------------

TEST_F(Procedures, SettingsReadDevolveVersaoEColecao)
{
    const auto reply = call("settings-read", Json::object());
    ASSERT_EQ(reply.rescode, Rescode::Ok);
    EXPECT_EQ(reply.payload["version"], 3);
    EXPECT_EQ(reply.payload["settings"][0]["filtered"], false);

    EXPECT_EQ(call("settings-read", {{"refs", {"broker.url"}}}).payload["settings"][0]["filtered"],
              true);
}

TEST_F(Procedures, SettingsReadPaginaComFromECountESemElesDevolveTudo)
{
    settings.extra = 69;  // 70 itens, como a coleção com as 64 saídas

    // Sem from/count: a coleção inteira e sem `more`, como sempre foi.
    auto reply = call("settings-read", Json::object());
    ASSERT_EQ(reply.rescode, Rescode::Ok);
    EXPECT_EQ(reply.payload["settings"].size(), 70U);
    EXPECT_FALSE(reply.payload.contains("more"));

    reply = call("settings-read", {{"from", 0}, {"count", 32}});
    ASSERT_EQ(reply.rescode, Rescode::Ok);
    EXPECT_EQ(reply.payload["version"], 3);
    ASSERT_EQ(reply.payload["settings"].size(), 32U);
    EXPECT_EQ(reply.payload["settings"][0]["name"], "broker.url");
    EXPECT_EQ(reply.payload["more"], true);

    reply = call("settings-read", {{"from", 64}, {"count", 32}});
    ASSERT_EQ(reply.payload["settings"].size(), 6U);
    EXPECT_EQ(reply.payload["settings"][0]["name"], "item.63");
    EXPECT_EQ(reply.payload["more"], false);

    reply = call("settings-read", {{"from", 32}});  // count ausente: até o teto
    EXPECT_EQ(reply.payload["settings"].size(), 38U);
    EXPECT_EQ(reply.payload["more"], false);

    reply = call("settings-read", {{"from", 500}, {"count", 10}});  // além do fim
    EXPECT_TRUE(reply.payload["settings"].empty());
    EXPECT_EQ(reply.payload["more"], false);

    // Com refs, a página é sobre a coleção filtrada.
    reply = call("settings-read", {{"refs", {"broker.url"}}, {"count", 1}});
    EXPECT_EQ(reply.payload["settings"][0]["filtered"], true);
    EXPECT_EQ(reply.payload["more"], true);

    EXPECT_EQ(call("settings-read", {{"from", -1}}).rescode, Rescode::BadRequest);
    EXPECT_EQ(call("settings-read", {{"count", 0}}).rescode, Rescode::BadRequest);
    EXPECT_EQ(call("settings-read", {{"count", 501}}).rescode, Rescode::BadRequest);
    EXPECT_EQ(call("settings-read", {{"count", "10"}}).rescode, Rescode::BadRequest);
    EXPECT_EQ(call("settings-read", {{"pagina", 1}}).rescode, Rescode::BadRequest);
}

TEST_F(Procedures, SettingsWriteAplicaEDevolveRecusas)
{
    settings.refuse = {{"device.id", "somente leitura"}};
    const Json item = {{"index", "P004"}, {"name", "broker.url"}, {"type", "text"},
                       {"value", "mqtt.dextro"}, {"default", ""}};
    const auto reply = call("settings-write", {{"version", 4}, {"settings", Json::array({item})}});
    ASSERT_EQ(reply.rescode, Rescode::Ok);
    EXPECT_EQ(reply.payload["appliedVersion"], 4);
    ASSERT_EQ(reply.payload["rejected"].size(), 1U);
    EXPECT_EQ(reply.payload["rejected"][0]["ref"], "device.id");
    EXPECT_EQ(session.reconnects, 0);
}

TEST_F(Procedures, SettingsWriteQueMexeNoBrokerPedeReconexao)
{
    settings.affects_session = true;
    ASSERT_EQ(call("settings-write", {{"version", 4}, {"settings", Json::array()}}).rescode,
              Rescode::Ok);
    EXPECT_EQ(session.reconnects, 1);
}

TEST_F(Procedures, SettingsWriteVersaoVelhaE409)
{
    const auto reply = call("settings-write", {{"version", 2}, {"settings", Json::array()}});
    EXPECT_EQ(reply.rescode, Rescode::Conflict);
    EXPECT_EQ(reply.payload["error"], "STALE_VERSION");
    EXPECT_TRUE(settings.received.is_null()) << "versão velha não pode chegar ao store";
}

TEST_F(Procedures, SettingsWriteMesmaVersaoEReentregaAceita)
{
    EXPECT_EQ(call("settings-write", {{"version", 3}, {"settings", Json::array()}}).rescode,
              Rescode::Ok);
}

TEST_F(Procedures, SettingsWriteComNomeRepetidoEInvalidSetting)
{
    const Json item = {{"index", "P004"}, {"name", "broker.url"}, {"type", "text"},
                       {"value", "a"}, {"default", ""}};
    Json other = item;
    other["index"] = "P005";
    const auto reply = call("settings-write", {{"version", 4}, {"settings", {item, other}}});
    EXPECT_EQ(reply.rescode, Rescode::BadRequest);
    EXPECT_EQ(reply.payload["error"], "INVALID_SETTING");
}

TEST_F(Procedures, SettingsWriteItemSemCampoObrigatorioE400)
{
    const Json item = {{"index", "P004"}, {"name", "broker.url"}};
    EXPECT_EQ(call("settings-write", {{"version", 4}, {"settings", Json::array({item})}}).rescode,
              Rescode::BadRequest);
}

TEST_F(Procedures, SettingsWriteQueNaoGravouE500)
{
    settings.persist = false;
    EXPECT_EQ(call("settings-write", {{"version", 4}, {"settings", Json::array()}}).rescode,
              Rescode::ServerError);
}

// --- certificate-request / certificate-install -------------------------------

TEST_F(Procedures, CertificateRequestDevolveCsrDoPedido)
{
    const auto reply = call("certificate-request",
                            {{"requestId", kUuid}, {"reason", "initial"}, {"keyType", "EC-P256"}});
    ASSERT_EQ(reply.rescode, Rescode::Ok);
    EXPECT_EQ(reply.payload["requestId"], kUuid);
    EXPECT_EQ(reply.payload["csrPem"].get<std::string>().rfind("-----BEGIN CERTIFICATE REQUEST-----", 0),
              0U);
    EXPECT_EQ(certificates.last_request, kUuid);
}

TEST_F(Procedures, CertificateRequestComOutraChaveE400)
{
    EXPECT_EQ(call("certificate-request",
                   {{"requestId", kUuid}, {"reason", "initial"}, {"keyType", "RSA-2048"}})
                  .rescode,
              Rescode::BadRequest);
    EXPECT_TRUE(certificates.last_request.empty());
}

TEST_F(Procedures, CertificateInstallResponeEPedeReconexao)
{
    const auto reply = call("certificate-install", {{"certificateId", kUuid},
                                                    {"certPem", "-----BEGIN CERTIFICATE-----"},
                                                    {"chainPem", ""},
                                                    {"notAfter", 1790000000000}});
    ASSERT_EQ(reply.rescode, Rescode::Ok);
    EXPECT_EQ(reply.payload["installed"], true);
    EXPECT_EQ(reply.payload["fingerprint"].get<std::string>().size(), 64U);
    EXPECT_EQ(session.reconnects, 1);
}

TEST_F(Procedures, CertificateInstallQueNaoCasaECsrInvalidSemReconexao)
{
    certificates.next = CertificateStore::Result::Mismatch;
    const auto reply = call("certificate-install", {{"certificateId", kUuid},
                                                    {"certPem", "x"},
                                                    {"chainPem", ""},
                                                    {"notAfter", 1}});
    EXPECT_EQ(reply.rescode, Rescode::BadRequest);
    EXPECT_EQ(reply.payload["error"], "CSR_INVALID");
    EXPECT_EQ(session.reconnects, 0);
}

TEST_F(Procedures, CertificateInstallComPemGrandeDemaisE400)
{
    EXPECT_EQ(call("certificate-install", {{"certificateId", kUuid},
                                           {"certPem", std::string(8193, 'x')},
                                           {"chainPem", ""},
                                           {"notAfter", 1}})
                  .rescode,
              Rescode::BadRequest);
}

// --- o contrato da classe abstrata -------------------------------------------

namespace {

class Throws final : public Procedure {
public:
    std::optional<Reply> validate(const Json&) const override { return std::nullopt; }
    Reply execute(const Request&) override { throw std::runtime_error("defeito no handler"); }
};

}  // namespace

TEST(IotProcedure, ExcecaoNoHandlerVira500EmVezDeDerrubar)
{
    Throws op;
    const auto reply = op.handle(Request{});
    EXPECT_EQ(reply.rescode, Rescode::ServerError);
    EXPECT_EQ(reply.payload["error"], "INTERNAL");
    EXPECT_EQ(reply.payload["detail"], "defeito no handler");
}

TEST(IotProcedure, PortAusenteE500Unavailable)
{
    ProcedureTable table;
    register_generic(table, nullptr, nullptr, nullptr, nullptr);
    for (const char* op : {"get-status", "settings-read", "outbox-notification"}) {
        Request request;
        if (std::string_view(op) == "outbox-notification") {
            request.payload = {{"entities", Json::array()}, {"size", 0}};
        }
        const auto reply = table.dispatch(op, request, false);
        EXPECT_EQ(reply.rescode, Rescode::ServerError) << op;
        EXPECT_EQ(reply.payload["error"], "UNAVAILABLE") << op;
    }
}

TEST(IotProcedureValidation, FormatosDoAnexoA)
{
    EXPECT_TRUE(is_uuid("0190f7a2-6c1e-7b3a-9f00-1234567890ab"));
    EXPECT_FALSE(is_uuid("0190f7a2-6c1e-7b3a-9f00-1234567890a"));
    EXPECT_FALSE(is_uuid("0190f7a2x6c1e-7b3a-9f00-1234567890ab"));

    EXPECT_TRUE(is_semver("1.2.3"));
    EXPECT_TRUE(is_semver("1.2.3-rc.1"));
    EXPECT_FALSE(is_semver("1.2"));
    EXPECT_FALSE(is_semver("1..3"));
    EXPECT_FALSE(is_semver("1.2.3-"));

    EXPECT_TRUE(is_version_token("20260927202307"));
    EXPECT_TRUE(is_version_token("1.2.3-rc.1+build_7"));
    EXPECT_FALSE(is_version_token(""));
    EXPECT_FALSE(is_version_token("1.2/3"));
    EXPECT_FALSE(is_version_token("1 2"));
    EXPECT_FALSE(is_version_token(std::string(65, 'a')));
    EXPECT_TRUE(is_version_token(std::string(64, 'a')));

    EXPECT_TRUE(is_sha256_hex(std::string(64, 'A')));
    EXPECT_FALSE(is_sha256_hex(std::string(63, 'a')));
    EXPECT_FALSE(is_sha256_hex(std::string(64, 'g')));
}
