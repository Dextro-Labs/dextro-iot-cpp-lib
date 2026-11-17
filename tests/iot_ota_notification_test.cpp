// `ota-notification` e os adaptadores por arquivo (inbox e chaveiro).
//
// Origem: os testes de ota-notification de tests/unit/iot_procedures_test.cpp
// do ssat-traffic-controller (origin/devel 67a0f77). Os casos do SSAT que eram
// regra fixa da op (`dxu` e `bin ssatctffwrm` não comparam com a versão da
// imagem) viraram casos da `InstalledVersion`; os do contrato (`apk`, `dxu`
// sem signature) e os dos adaptadores são novos.
#include <dextro-iot/ota/event_sink.hpp>
#include <dextro-iot/procedures/ops/ota_notification.hpp>
#include <dextro-iot/procedures/ota_adapters.hpp>
#include <dextro-iot/procedures/procedure_table.hpp>

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>

#ifndef DEXTRO_IOT_TEST_DIR
#error "DEXTRO_IOT_TEST_DIR não definido"
#endif

using namespace dextro::iot::procedures;

namespace {

const std::string kUuid = "0190f7a2-6c1e-7b3a-9f00-1234567890ab";
const std::string kSignature = std::string(86, 'A') + "==";

class FakeSpool final : public OtaSpool {
public:
    bool stored = true;
    std::vector<Json> queue;
    bool enqueue(const Json& manifest) override
    {
        queue.push_back(manifest);
        return stored;
    }
};

class FakeKeyring final : public Keyring {
public:
    bool accept = true;
    mutable std::vector<std::string> asked;
    bool known(std::string_view key_id) const override
    {
        asked.emplace_back(key_id);
        return accept;
    }
};

/// A da imagem para tudo, menos o firmware de uma placa que só o agente sabe
/// (no SSAT: `dxu` e `bin ssatctffwrm`).
class FakeInstalled final : public InstalledVersion {
public:
    std::string image = "0.1.0";
    std::optional<std::string> installed_version(const Json& manifest) const override
    {
        const std::string format = manifest.value("artifactFormat", std::string{});
        if (format == "dxu" ||
            (format == "bin" && manifest.value("artifactName", std::string{}) == "ssatctffwrm")) {
            return std::nullopt;
        }
        return image;
    }
};

class FakeEvents final : public dextro::iot::ota::EventSink {
public:
    std::vector<Json> events;
    bool emit(std::string_view code, const Json& args, std::int64_t occurred_ms) override
    {
        events.push_back({{"code", std::string(code)}, {"args", args}, {"at", occurred_ms}});
        return true;
    }
};

class OtaNotify : public ::testing::Test {
protected:
    void SetUp() override
    {
        table.add({"ota-notification", 1, false, 30000, false},
                  std::make_unique<OtaNotification>(&spool, &keyring, &installed));
    }
    Reply call(Json payload)
    {
        Request rq;
        rq.relation_id = "r-1";
        rq.payload = std::move(payload);
        return table.dispatch("ota-notification", rq, false);
    }
    static Json manifest()
    {
        return {{"version", "0.2.0"},
                {"artifactUrl", "https://artifacts.dextrolabs.com.br/x.ipk"},
                {"sha256", std::string(64, 'a')},
                {"signature", kSignature},
                {"signingKeyId", "k-2026"},
                {"sizeBytes", 1024},
                {"artifactFormat", "swu"},
                {"applyAfter", 0},
                {"campaignItemId", kUuid}};
    }

    FakeSpool spool;
    FakeKeyring keyring;
    FakeInstalled installed;
    ProcedureTable table;
};

}  // namespace

TEST_F(OtaNotify, ValidoVaiParaOSpoolE202)
{
    const auto reply = call(manifest());
    EXPECT_EQ(reply.rescode, Rescode::Accepted);
    ASSERT_EQ(spool.queue.size(), 1U);
    EXPECT_EQ(spool.queue[0]["campaignItemId"], kUuid);
    EXPECT_EQ(keyring.asked, (std::vector<std::string>{"k-2026"}));
}

TEST_F(OtaNotify, VersaoInstaladaE200SemSpool)
{
    Json m = manifest();
    m["version"] = "0.1.0";
    const auto reply = call(m);
    EXPECT_EQ(reply.rescode, Rescode::Ok);
    EXPECT_EQ(reply.payload["alreadyInstalled"], true);
    EXPECT_TRUE(spool.queue.empty());
}

TEST_F(OtaNotify, SemHttpsE400)
{
    Json m = manifest();
    m["artifactUrl"] = "http://artifacts.dextrolabs.com.br/x.ipk";
    EXPECT_EQ(call(m).rescode, Rescode::BadRequest);
    EXPECT_TRUE(spool.queue.empty());
}

TEST_F(OtaNotify, Sha256MalFormadoEChecksumInvalid)
{
    Json m = manifest();
    m["sha256"] = "abc";
    const auto reply = call(m);
    EXPECT_EQ(reply.rescode, Rescode::BadRequest);
    EXPECT_EQ(reply.payload["error"], "CHECKSUM_INVALID");
}

TEST_F(OtaNotify, AssinaturaQueNaoEEd25519ESignatureInvalid)
{
    Json m = manifest();
    m["signature"] = "curta";
    const auto reply = call(m);
    EXPECT_EQ(reply.rescode, Rescode::BadRequest);
    EXPECT_EQ(reply.payload["error"], "SIGNATURE_INVALID");
}

TEST_F(OtaNotify, ChaveDesconhecidaEUnknownSigningKey)
{
    keyring.accept = false;
    const auto reply = call(manifest());
    EXPECT_EQ(reply.rescode, Rescode::BadRequest);
    EXPECT_EQ(reply.payload["error"], "UNKNOWN_SIGNING_KEY");
    EXPECT_TRUE(spool.queue.empty());
}

TEST_F(OtaNotify, FormatoForaDaListaE400)
{
    Json m = manifest();
    m["artifactFormat"] = "zip";
    EXPECT_EQ(call(m).rescode, Rescode::BadRequest);
}

TEST_F(OtaNotify, ApkDoContratoEAceito)
{
    Json m = manifest();
    m["artifactFormat"] = "apk";
    EXPECT_EQ(call(m).rescode, Rescode::Accepted);
}

TEST_F(OtaNotify, SemCampoObrigatorioE400)
{
    for (const char* key : {"campaignItemId", "version", "artifactUrl", "sha256"}) {
        Json m = manifest();
        m.erase(key);
        EXPECT_EQ(call(m).rescode, Rescode::BadRequest) << key;
    }
    Json m = manifest();
    m["campaignItemId"] = "nao-e-uuid";
    EXPECT_EQ(call(m).rescode, Rescode::BadRequest);
    m = manifest();
    m["extra"] = 1;
    EXPECT_EQ(call(m).rescode, Rescode::BadRequest) << "additionalProperties: false";
    EXPECT_TRUE(spool.queue.empty());
}

TEST_F(OtaNotify, AceitaCarimboDaImagemComoVersao)
{
    Json m = manifest();
    m["version"] = "20260927202307";
    EXPECT_EQ(call(m).rescode, Rescode::Accepted);
}

TEST_F(OtaNotify, VersaoForaDoAlfabetoE400)
{
    Json m = manifest();
    m["version"] = "1.0 beta";
    EXPECT_EQ(call(m).rescode, Rescode::BadRequest);
    m["version"] = std::string(65, '1');
    EXPECT_EQ(call(m).rescode, Rescode::BadRequest);
    EXPECT_TRUE(spool.queue.empty());
}

TEST_F(OtaNotify, RaucbEDxuSemAssinaturaSaoAceitos)
{
    for (const char* format : {"raucb", "dxu"}) {
        Json m = manifest();
        m.erase("signature");
        m.erase("signingKeyId");
        m["artifactFormat"] = format;
        EXPECT_EQ(call(m).rescode, Rescode::Accepted) << format;
    }
    EXPECT_EQ(spool.queue.size(), 2U);
}

TEST_F(OtaNotify, OutrosFormatosExigemAssinatura)
{
    for (const char* format : {"bin", "swu", "tar.zst", "apk"}) {
        Json m = manifest();
        m.erase("signature");
        m["artifactFormat"] = format;
        const auto reply = call(m);
        EXPECT_EQ(reply.rescode, Rescode::BadRequest) << format;
        EXPECT_EQ(reply.payload["error"], "SIGNATURE_INVALID") << format;
    }
    Json sem_formato = manifest();
    sem_formato.erase("signature");
    sem_formato.erase("artifactFormat");
    EXPECT_EQ(call(sem_formato).rescode, Rescode::BadRequest);
    EXPECT_TRUE(spool.queue.empty());
}

TEST_F(OtaNotify, QuemNaoSabeAVersaoNaoRespondeAlreadyInstalled)
{
    // O firmware de uma placa: coincidir com a versão da imagem não diz nada.
    Json m = manifest();
    m["artifactFormat"] = "bin";
    m["artifactName"] = "ssatctffwrm";
    m["version"] = installed.image;
    EXPECT_EQ(call(m).rescode, Rescode::Accepted);
    ASSERT_EQ(spool.queue.size(), 1U);
    EXPECT_EQ(spool.queue.front()["signature"], kSignature);

    m["artifactName"] = "outrofw";
    const auto reply = call(m);
    EXPECT_EQ(reply.rescode, Rescode::Ok);
    EXPECT_EQ(reply.payload["alreadyInstalled"], true);
}

TEST_F(OtaNotify, SpoolQueNaoGravaE500Storage)
{
    spool.stored = false;
    const auto reply = call(manifest());
    EXPECT_EQ(reply.rescode, Rescode::ServerError);
    EXPECT_EQ(reply.payload["error"], "STORAGE");
}

TEST(OtaNotifySemPortas, SemSpoolE500ESemChaveiroAceitaQualquerChave)
{
    ProcedureTable t;
    t.add({"ota-notification", 1, false, 30000, false},
          std::make_unique<OtaNotification>(nullptr, nullptr, nullptr));
    Request rq;
    rq.payload = {{"version", "1"},
                  {"artifactUrl", "https://a/b"},
                  {"sha256", std::string(64, 'b')},
                  {"artifactFormat", "raucb"},
                  {"campaignItemId", kUuid}};
    EXPECT_EQ(t.dispatch("ota-notification", rq, false).rescode, Rescode::ServerError);

    FakeSpool spool;
    ProcedureTable t2;
    OtaNotification::Options only_raucb;
    only_raucb.formats = {"raucb"};
    t2.add({"ota-notification", 1, false, 30000, false},
           std::make_unique<OtaNotification>(&spool, nullptr, nullptr, only_raucb));
    rq.payload["signingKeyId"] = "qualquer";
    EXPECT_EQ(t2.dispatch("ota-notification", rq, false).rescode, Rescode::Accepted);
    rq.payload["artifactFormat"] = "dxu";
    EXPECT_EQ(t2.dispatch("ota-notification", rq, false).rescode, Rescode::BadRequest);
}

/* -------------------------------------------------------- adaptadores --- */

namespace {

std::string tmp(const char* name)
{
    return std::string(DEXTRO_IOT_TEST_DIR) + "/on-" + name + "-" + std::to_string(::getpid());
}

}  // namespace

TEST(OtaInbox, GravaOManifestoEEmiteOtaReceived)
{
    const std::string dir = tmp("inbox") + "/a/b";
    FakeEvents events;
    InboxOtaSpool inbox(dir, &events);
    Json m = {{"campaignItemId", kUuid}, {"version", "20260927202307"}};
    ASSERT_TRUE(inbox.enqueue(m));
    std::ifstream in(dir + "/" + kUuid + ".json");
    ASSERT_TRUE(in.good());
    EXPECT_EQ(Json::parse(in), m);
    ASSERT_EQ(events.events.size(), 1U);
    EXPECT_EQ(events.events[0]["code"], "ota-received");
    EXPECT_EQ(events.events[0]["args"]["campaignItemId"], kUuid);
    EXPECT_EQ(events.events[0]["args"]["version"], "20260927202307");
    EXPECT_EQ(events.events[0]["args"]["applyAfter"], 0) << "o schema do evento 40 exige applyAfter";
    EXPECT_GT(events.events[0]["at"].get<std::int64_t>(), 0);

    // Reentrega: o mesmo arquivo.
    m["applyAfter"] = 1790590000000LL;
    ASSERT_TRUE(inbox.enqueue(m));
    EXPECT_EQ(events.events[1]["args"]["applyAfter"], 1790590000000LL);

    EXPECT_FALSE(inbox.enqueue({{"campaignItemId", "../x"}}));
    std::remove((dir + "/" + kUuid + ".json").c_str());
    ::rmdir(dir.c_str());
    ::rmdir((tmp("inbox") + "/a").c_str());
    ::rmdir(tmp("inbox").c_str());
}

TEST(OtaChaveiro, SemDiretorioAceitaComDiretorioConfere)
{
    const std::string dir = tmp("keys");
    DirectoryKeyring keys(dir);
    EXPECT_TRUE(keys.known("qualquer")) << "sem chaveiro na imagem a recusa é do agente";
    ::mkdir(dir.c_str(), 0755);
    EXPECT_FALSE(keys.known("k1"));
    std::ofstream(dir + "/k1.pub") << "pem";
    EXPECT_TRUE(keys.known("k1"));
    EXPECT_FALSE(keys.known("../k1"));
    EXPECT_FALSE(keys.known(".k1"));
    std::remove((dir + "/k1.pub").c_str());
    ::rmdir(dir.c_str());
}
