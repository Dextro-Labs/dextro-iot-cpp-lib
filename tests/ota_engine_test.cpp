// A máquina de estados do OTA contra portas falsas: sem rede, sem rauc, sem
// placa.
//
// Origem: tests/unit/engine_test.cpp do ssat-ota-agent (origin/devel 76aa940),
// a parte `raucb` inteira (a pasta de saída virou o `EventSink` falso). Os
// testes dos formatos `dxu`/`bin` do SSAT ficaram lá com os handlers deles; os
// de handler genérico e de assinatura são novos.
#include <dextro-iot/ota/engine.hpp>
#include <dextro-iot/ota/fs.hpp>
#include <dextro-iot/ota/rauc.hpp>
#include <dextro-iot/ota/sha256.hpp>

#include "ota_test_keys.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <deque>
#include <fstream>

#ifndef DEXTRO_IOT_TEST_DIR
#error "DEXTRO_IOT_TEST_DIR não definido"
#endif

namespace {

using namespace dextro::iot::ota;
using Json = nlohmann::json;

const std::string kId = "0190f7a2-6c1e-7b3a-9f00-1234567890ab";
const std::string kBundle = "conteudo do bundle RAUC, de mentira";

/// O servidor: entrega `content`, em pedaços, retomando do que o `.part` já
/// tem. Cada item de `script` é o que acontece numa chamada.
class FakeDownloader final : public Downloader {
public:
    enum class Step { All, HalfThenDrop, Forbidden };
    std::string content = kBundle;
    std::deque<Step> script;
    std::vector<std::int64_t> resumed_from;
    std::vector<std::string> tokens;

    DownloadResult fetch(const DownloadRequest& req, const KeepGoing&) override
    {
        const std::int64_t have = std::max<std::int64_t>(fs::file_size(req.path), 0);
        resumed_from.push_back(have);
        tokens.push_back(req.token);
        const Step step = script.empty() ? Step::All : script.front();
        if (!script.empty()) {
            script.pop_front();
        }
        DownloadResult r;
        if (step == Step::Forbidden) {
            r.outcome = DownloadResult::Outcome::Permanent;
            r.code = "AUTH_REJECTED";
            r.message = "HTTP 403";
            return r;
        }
        const std::size_t from = static_cast<std::size_t>(have);
        const std::size_t to = step == Step::All ? content.size() : content.size() / 2;
        std::ofstream(req.path, std::ios::app | std::ios::binary) << content.substr(from, to - from);
        r.total_bytes = fs::file_size(req.path);
        r.outcome = step == Step::All ? DownloadResult::Outcome::Ok
                                      : DownloadResult::Outcome::Transient;
        if (step != Step::All) {
            r.code = "DOWNLOAD_FAILED";
            r.message = "conexão caiu";
        }
        return r;
    }
};

class FakeInstaller final : public Installer {
public:
    bool ok = true;
    std::string expected = kBundle;
    std::vector<std::string> bundles;
    bool install(const std::string& bundle, std::string& message, const KeepGoing&) override
    {
        bundles.push_back(bundle);
        std::string text;
        fs::read_file(bundle, text);
        EXPECT_EQ(text, expected);
        if (!ok) {
            message = "rauc install saiu com 1: signature verification failed";
        }
        return ok;
    }
};

class FakePlatform final : public Platform {
public:
    std::string boot = "boot-1";
    std::string slot = "A";
    MarkGood mark = MarkGood::Pending;
    bool bad = false;
    bool reboot_ok = true;
    int reboots = 0;
    std::int64_t free = 1LL << 40;
    std::int64_t wall = 1790590000000LL - 60000;
    std::int64_t mono = 1000;
    std::string token = "dxd_teste";

    std::string boot_id() override { return boot; }
    std::string current_slot() override { return slot; }
    std::string other_slot(const std::string& current) override
    {
        return current == "A" ? "B" : current == "B" ? "A" : "";
    }
    MarkGood mark_good() override { return mark; }
    bool slot_bad(const std::string&) override { return bad; }
    bool reboot() override
    {
        ++reboots;
        return reboot_ok;
    }
    std::int64_t free_bytes(const std::string&) override { return free; }
    std::int64_t wall_ms() override { return wall; }
    std::int64_t mono_ms() override { return mono; }
    std::string device_token() override { return token; }
};

/// A fila de eventos da aplicação, de mentira.
class FakeEvents final : public EventSink {
public:
    std::vector<Json> events;
    bool emit(std::string_view code, const Json& args, std::int64_t occurred_ms) override
    {
        events.push_back({{"code", std::string(code)}, {"args", args}, {"occurredAt", occurred_ms}});
        return true;
    }
};

class Ota : public ::testing::Test {
protected:
    void SetUp() override
    {
        root = std::string(DEXTRO_IOT_TEST_DIR) + "/ota-" +
               ::testing::UnitTest::GetInstance()->current_test_info()->name() + "-" +
               std::to_string(::getpid());
        paths = Engine::Paths::under(root, root + "/keys");
        wipe();
        fs::make_dirs(paths.inbox);
    }
    void TearDown() override { wipe(); }
    void wipe()
    {
        for (const std::string& d : {paths.inbox, paths.state, paths.download, paths.keyring}) {
            for (const std::string& n : fs::list(d, "")) {
                fs::remove_file(d + "/" + n);
            }
            ::rmdir(d.c_str());
        }
        ::rmdir(root.c_str());
    }

    std::unique_ptr<Engine> engine()
    {
        auto e = std::make_unique<Engine>(paths, downloader, platform, sink);
        e->add_handler(std::make_unique<RaucHandler>(installer));
        return e;
    }

    Json manifest(const std::string& content = kBundle)
    {
        return {{"version", "20260927202307"},
                {"artifactUrl", "https://api.dextrolabs.com.br/v1/devices/artifact-download/x/img?version=1"},
                {"sha256", sha_of(content)},
                {"sizeBytes", static_cast<std::int64_t>(content.size())},
                {"artifactFormat", "raucb"},
                {"applyAfter", 1790590000000LL},
                {"campaignItemId", kId}};
    }
    std::string sha_of(const std::string& content)
    {
        const std::string p = root + "-sha";
        std::ofstream(p) << content;
        const auto s = *sha256_file(p);
        fs::remove_file(p);
        return s;
    }
    void deliver(const Json& m) { fs::write_atomic(paths.inbox + "/" + kId + ".json", m.dump()); }

    /// Os eventos emitidos desde a última chamada, em ordem.
    std::vector<Json> drain()
    {
        std::vector<Json> out;
        out.swap(sink.events);
        return out;
    }
    static std::vector<std::string> codes(const std::vector<Json>& events)
    {
        std::vector<std::string> c;
        for (const Json& e : events) {
            c.push_back(e["code"]);
        }
        return c;
    }
    bool has_state() { return fs::exists(paths.state + "/" + kId + ".json"); }
    bool has_inbox() { return fs::exists(paths.inbox + "/" + kId + ".json"); }
    Stage stage()
    {
        std::string text;
        fs::read_file(paths.state + "/" + kId + ".json", text);
        return Job::from_json(Json::parse(text))->stage;
    }

    Engine::Paths paths;
    std::string root;
    FakeDownloader downloader;
    FakeInstaller installer;
    FakePlatform platform;
    FakeEvents sink;
};

}  // namespace

/* ------------------------------------------------------------ raucb --- */

TEST_F(Ota, CaminhoFelizAtravessaOReboot)
{
    deliver(manifest());
    {
        auto e = engine();
        e->tick();
        EXPECT_TRUE(downloader.resumed_from.empty()) << "baixou antes do applyAfter";
        EXPECT_EQ(stage(), Stage::Received);

        platform.wall = 1790590000000LL;
        e->tick();
        EXPECT_EQ(platform.reboots, 1);
        EXPECT_EQ(stage(), Stage::Rebooting);
    }
    auto ev = drain();
    EXPECT_EQ(codes(ev), (std::vector<std::string>{"ota-download-started", "ota-download-completed",
                                                    "ota-verified", "ota-install-started",
                                                    "ota-installed", "ota-rebooting"}));
    for (const Json& x : ev) {
        EXPECT_EQ(x["args"]["campaignItemId"], kId);
        EXPECT_EQ(x["args"]["version"], "20260927202307");
        EXPECT_EQ(x["occurredAt"], 1790590000000LL);
    }
    EXPECT_EQ(ev[0]["args"]["attempt"], 1);
    EXPECT_EQ(ev[0]["args"]["sizeBytes"], static_cast<std::int64_t>(kBundle.size()));
    EXPECT_EQ(ev[1]["args"]["bytes"], static_cast<std::int64_t>(kBundle.size()));
    EXPECT_EQ(ev[2]["args"]["sha256"], sha_of(kBundle));
    EXPECT_EQ(ev[3]["args"]["slot"], "B");
    EXPECT_EQ(ev[4]["args"]["slot"], "B");
    EXPECT_EQ(downloader.tokens[0], "dxd_teste");
    EXPECT_FALSE(fs::exists(paths.download + "/" + kId + ".raucb")) << "o bundle fica ocupando o /data";

    // O boot novo: outro boot_id, o slot B, e um processo novo lendo o estado.
    platform.boot = "boot-2";
    platform.slot = "B";
    auto after = engine();
    after->tick();
    EXPECT_EQ(stage(), Stage::Confirming);
    EXPECT_TRUE(drain().empty()) << "completed antes do mark-good";

    platform.mark = MarkGood::Good;
    after->tick();
    ev = drain();
    ASSERT_EQ(codes(ev), (std::vector<std::string>{"ota-completed"}));
    EXPECT_EQ(ev[0]["args"]["slot"], "B");
    EXPECT_FALSE(has_state());
    EXPECT_FALSE(has_inbox());
    EXPECT_EQ(platform.reboots, 1);
}

TEST_F(Ota, DownloadRetomaDeOndeParouComBackoff)
{
    platform.wall = 1790590000000LL;
    downloader.script = {FakeDownloader::Step::HalfThenDrop, FakeDownloader::Step::All};
    deliver(manifest());
    auto e = engine();
    e->tick();
    EXPECT_EQ(stage(), Stage::Downloading);
    EXPECT_EQ(codes(drain()), (std::vector<std::string>{"ota-download-started"}));

    // Um processo novo (o agente reiniciou) antes da hora: espera.
    auto again = engine();
    again->tick();
    EXPECT_EQ(downloader.resumed_from.size(), 1U);

    platform.wall += Engine::backoff_ms(1);
    again->tick();
    ASSERT_EQ(downloader.resumed_from.size(), 2U);
    EXPECT_EQ(downloader.resumed_from[1], static_cast<std::int64_t>(kBundle.size() / 2));
    const auto ev = drain();
    ASSERT_GE(ev.size(), 2U);
    EXPECT_EQ(ev[0]["code"], "ota-download-started");
    EXPECT_EQ(ev[0]["args"]["attempt"], 2);
    EXPECT_EQ(ev[1]["code"], "ota-download-completed");
    EXPECT_EQ(stage(), Stage::Rebooting);
}

TEST_F(Ota, DownloadEsgotadoViraFailedRetryable)
{
    platform.wall = 1790590000000LL;
    for (int i = 0; i < Engine::kMaxDownloadAttempts; ++i) {
        downloader.script.push_back(FakeDownloader::Step::HalfThenDrop);
    }
    // A conexão cai sempre na metade; o .part é apagado a cada volta para a
    // metade nunca virar o arquivo inteiro.
    deliver(manifest());
    auto e = engine();
    for (int i = 0; i < Engine::kMaxDownloadAttempts; ++i) {
        fs::remove_file(paths.download + "/" + kId + ".raucb.part");
        e->tick();
        platform.wall += Engine::backoff_ms(i + 1);
    }
    const auto ev = drain();
    ASSERT_FALSE(ev.empty());
    EXPECT_EQ(ev.back()["code"], "ota-failed");
    EXPECT_EQ(ev.back()["args"]["stage"], "download");
    EXPECT_EQ(ev.back()["args"]["retryable"], true);
    EXPECT_FALSE(has_state());
    EXPECT_FALSE(has_inbox());
}

TEST_F(Ota, Sha256ErradoFalhaNaVerificacaoEApagaOArquivo)
{
    platform.wall = 1790590000000LL;
    Json m = manifest();
    m["sha256"] = std::string(64, '0');
    deliver(m);
    engine()->tick();
    const auto ev = drain();
    ASSERT_EQ(codes(ev), (std::vector<std::string>{"ota-download-started", "ota-download-completed",
                                                    "ota-failed"}));
    EXPECT_EQ(ev[2]["args"]["stage"], "verify");
    EXPECT_EQ(ev[2]["args"]["code"], "CHECKSUM_MISMATCH");
    EXPECT_EQ(ev[2]["args"]["retryable"], true);
    EXPECT_TRUE(installer.bundles.empty());
    EXPECT_TRUE(fs::list(paths.download, "").empty());
    EXPECT_FALSE(has_inbox());
}

TEST_F(Ota, TamanhoDiferenteDoManifestoFalhaNaVerificacao)
{
    platform.wall = 1790590000000LL;
    Json m = manifest();
    m["sizeBytes"] = static_cast<std::int64_t>(kBundle.size() + 1);
    deliver(m);
    engine()->tick();
    const auto ev = drain();
    ASSERT_FALSE(ev.empty());
    EXPECT_EQ(ev.back()["args"]["code"], "SIZE_MISMATCH");
}

TEST_F(Ota, RaucRecusouViraInstallFailedSemRetry)
{
    platform.wall = 1790590000000LL;
    installer.ok = false;
    deliver(manifest());
    engine()->tick();
    const auto ev = drain();
    ASSERT_EQ(ev.back()["code"], "ota-failed");
    EXPECT_EQ(ev.back()["args"]["stage"], "install");
    EXPECT_EQ(ev.back()["args"]["code"], "RAUC_INSTALL_FAILED");
    EXPECT_EQ(ev.back()["args"]["retryable"], false);
    EXPECT_NE(ev.back()["args"]["message"].get<std::string>().find("signature"), std::string::npos);
    EXPECT_EQ(platform.reboots, 0);
    EXPECT_TRUE(fs::list(paths.download, "").empty());
}

TEST_F(Ota, BootForaDeUmSlotNaoInstala)
{
    platform.wall = 1790590000000LL;
    platform.slot.clear();  // recovery
    deliver(manifest());
    engine()->tick();
    const auto ev = drain();
    ASSERT_EQ(ev.back()["code"], "ota-failed");
    EXPECT_EQ(ev.back()["args"]["stage"], "install");
    EXPECT_EQ(ev.back()["args"]["code"], "SLOT_UNKNOWN");
    EXPECT_TRUE(installer.bundles.empty());
}

TEST_F(Ota, VoltouAoSlotAntigoERollback)
{
    platform.wall = 1790590000000LL;
    deliver(manifest());
    engine()->tick();
    drain();
    platform.boot = "boot-4";  // três tentativas e o bootloader desistiu
    platform.bad = true;
    engine()->tick();
    const auto ev = drain();
    ASSERT_EQ(codes(ev), (std::vector<std::string>{"ota-rolled-back"}));
    EXPECT_EQ(ev[0]["args"]["fromSlot"], "B");
    EXPECT_EQ(ev[0]["args"]["toSlot"], "A");
    EXPECT_EQ(ev[0]["args"]["reason"], "bootloader-giveup");
    EXPECT_FALSE(has_state());
}

TEST_F(Ota, SlotDaCmdlineEmOutraCaixaNaoEhRollback)
{
    platform.wall = 1790590000000LL;
    deliver(manifest());
    engine()->tick();
    drain();
    platform.boot = "boot-2";
    platform.slot = "b";  // `ssat.slot=b` e o bootname `B`
    platform.mark = MarkGood::Good;
    engine()->tick();
    EXPECT_EQ(codes(drain()), (std::vector<std::string>{"ota-completed"}));
}

TEST_F(Ota, MesmoBootNaoConfirmaEPedeORebootDeNovo)
{
    platform.wall = 1790590000000LL;
    deliver(manifest());
    engine()->tick();
    drain();
    // O agente reiniciou sem a placa reiniciar: o slot novo só sobe com reboot.
    platform.wall += 60000;
    engine()->tick();
    EXPECT_EQ(platform.reboots, 2);
    EXPECT_EQ(stage(), Stage::Rebooting);
    EXPECT_TRUE(drain().empty());
}

TEST_F(Ota, RebootQueNaoAconteceEContadoUmaVezEOItemContinua)
{
    platform.wall = 1790590000000LL;
    platform.reboot_ok = false;
    deliver(manifest());
    auto e = engine();
    for (int i = 0; i < 5; ++i) {
        e->tick();
        platform.wall += 60000;
    }
    const auto ev = drain();
    int failed = 0;
    for (const Json& x : ev) {
        if (x["code"] == "ota-failed") {
            ++failed;
            EXPECT_EQ(x["args"]["stage"], "reboot");
            EXPECT_EQ(x["args"]["code"], "REBOOT_FAILED");
        }
    }
    EXPECT_EQ(failed, 1);
    EXPECT_EQ(stage(), Stage::Rebooting);
    // Um reboot manual depois: confirma normalmente.
    platform.boot = "boot-2";
    platform.slot = "B";
    platform.mark = MarkGood::Good;
    e->tick();
    EXPECT_EQ(codes(drain()), (std::vector<std::string>{"ota-completed"}));
}

TEST_F(Ota, MarkGoodQueNuncaFechaViraTimeout)
{
    platform.wall = 1790590000000LL;
    deliver(manifest());
    engine()->tick();
    drain();
    platform.boot = "boot-2";
    platform.slot = "B";
    auto after = engine();
    after->tick();
    platform.mono += Engine::kConfirmTimeoutMs;
    after->tick();
    const auto ev = drain();
    ASSERT_EQ(codes(ev), (std::vector<std::string>{"ota-failed"}));
    EXPECT_EQ(ev[0]["args"]["stage"], "mark-good");
    EXPECT_EQ(ev[0]["args"]["code"], "MARK_GOOD_TIMEOUT");
}

TEST_F(Ota, MarkGoodQueFalhaSemReiniciarViraFailed)
{
    platform.wall = 1790590000000LL;
    deliver(manifest());
    engine()->tick();
    drain();
    platform.boot = "boot-2";
    platform.slot = "B";
    platform.mark = MarkGood::Failed;
    auto after = engine();
    after->tick();
    EXPECT_TRUE(drain().empty()) << "a placa ainda pode reiniciar sozinha";
    platform.mono += Engine::kMarkGoodFailedGraceMs;
    after->tick();
    const auto ev = drain();
    ASSERT_EQ(codes(ev), (std::vector<std::string>{"ota-failed"}));
    EXPECT_EQ(ev[0]["args"]["code"], "MARK_GOOD_FAILED");
}

TEST_F(Ota, ReentregaDuranteOItemNaoAbreOutroEDepoisDoFimAbre)
{
    downloader.script = {FakeDownloader::Step::HalfThenDrop};
    platform.wall = 1790590000000LL;
    deliver(manifest());
    auto e = engine();
    e->tick();
    ASSERT_EQ(stage(), Stage::Downloading);
    deliver(manifest());  // o backend mandou de novo (outbox)
    e->tick();
    EXPECT_EQ(stage(), Stage::Downloading);
    EXPECT_EQ(downloader.resumed_from.size(), 1U) << "a reentrega não pode recomeçar o item";

    // Termina com falha; o retry do backend (mesmo id) abre uma tentativa nova.
    installer.ok = false;
    platform.wall += Engine::backoff_ms(1);
    e->tick();
    EXPECT_FALSE(has_state());
    drain();
    installer.ok = true;
    deliver(manifest());
    e->tick();
    EXPECT_EQ(stage(), Stage::Rebooting);
}

TEST_F(Ota, FormatoSemHandlerNaoBaixa)
{
    Json m = manifest();
    m["artifactFormat"] = "tar.zst";
    deliver(m);
    engine()->tick();
    const auto ev = drain();
    ASSERT_EQ(codes(ev), (std::vector<std::string>{"ota-failed"}));
    EXPECT_EQ(ev[0]["args"]["stage"], "install");
    EXPECT_EQ(ev[0]["args"]["code"], "FORMAT_UNSUPPORTED");
    EXPECT_EQ(ev[0]["args"]["retryable"], false);
    EXPECT_NE(ev[0]["args"]["message"].get<std::string>().find("raucb"), std::string::npos);
    EXPECT_TRUE(downloader.resumed_from.empty());
}

TEST_F(Ota, SemEspacoESemTokenFalhamAntesDeBaixar)
{
    platform.wall = 1790590000000LL;
    platform.free = 1024;
    deliver(manifest());
    engine()->tick();
    auto ev = drain();
    ASSERT_EQ(codes(ev), (std::vector<std::string>{"ota-failed"}));
    EXPECT_EQ(ev[0]["args"]["code"], "NO_SPACE");

    platform.free = 1LL << 40;
    platform.token.clear();
    deliver(manifest());
    engine()->tick();
    ev = drain();
    ASSERT_EQ(codes(ev), (std::vector<std::string>{"ota-failed"}));
    EXPECT_EQ(ev[0]["args"]["code"], "NO_DEVICE_TOKEN");
    EXPECT_TRUE(downloader.resumed_from.empty());
}

TEST_F(Ota, ManifestoInvalidoComIdViraFailedERejected)
{
    Json m = manifest();
    m["sha256"] = "curto";
    deliver(m);
    engine()->tick();
    const auto ev = drain();
    ASSERT_EQ(codes(ev), (std::vector<std::string>{"ota-failed"}));
    EXPECT_EQ(ev[0]["args"]["code"], "MANIFEST_INVALID");
    EXPECT_EQ(ev[0]["args"]["version"], "20260927202307");
    EXPECT_FALSE(has_inbox());
    EXPECT_TRUE(fs::exists(paths.inbox + "/" + kId + ".json.rejected"));
}

TEST_F(Ota, TokenRecusadoFalhaSemRetry)
{
    platform.wall = 1790590000000LL;
    downloader.script = {FakeDownloader::Step::Forbidden};
    deliver(manifest());
    engine()->tick();
    const auto ev = drain();
    ASSERT_EQ(ev.back()["args"]["code"], "AUTH_REJECTED");
    EXPECT_EQ(ev.back()["args"]["retryable"], false);
}

TEST_F(Ota, RaucIgnoraASignatureDoManifesto)
{
    // O bundle RAUC se assina sozinho (CMS, conferido pelo `rauc install`):
    // uma `signature` no manifesto não é conferida contra o chaveiro.
    platform.wall = 1790590000000LL;
    Json m = manifest();
    m["signature"] = std::string(86, 'A') + "==";
    m["signingKeyId"] = "chave-que-nao-existe";
    deliver(m);
    engine()->tick();
    EXPECT_EQ(stage(), Stage::Rebooting);
}

/* ------------------------------------------- handler genérico, sem reboot --- */

namespace {

/// Um formato entregue a "outro daemon": pede uma vez, acompanha, conclui.
class FakeHandler final : public ArtifactHandler {
public:
    Signature policy = Signature::IfPresent;
    std::optional<Failure> refuse_accept;
    std::optional<Failure> refuse_check;
    std::deque<InstallStep> script;
    int requests = 0;  ///< "pedidos" ao outro daemon
    int calls = 0;

    std::string format() const override { return "fw"; }
    std::string extension() const override { return "fwbin"; }
    Signature signature() const override { return policy; }
    std::optional<Failure> accept(const Manifest&) override { return refuse_accept; }
    std::optional<Failure> check(const Manifest&, const std::string& file) override
    {
        EXPECT_TRUE(fs::exists(file));
        return refuse_check;
    }
    InstallStep install(InstallContext& ctx) override
    {
        ++calls;
        if (!ctx.job.handler.value("requested", false)) {
            ++requests;
            ctx.job.handler["requested"] = true;
            ctx.save();
            ctx.emit("ota-install-started", {{"slot", "mcu"}});
        }
        if (script.empty()) {
            return InstallStep::pending(2000);
        }
        InstallStep s = script.front();
        script.pop_front();
        return s;
    }
};

class OtaHandler : public Ota {
protected:
    std::unique_ptr<Engine> engine_with(FakeHandler*& out)
    {
        auto e = engine();
        auto h = std::make_unique<FakeHandler>();
        out = h.get();
        EXPECT_TRUE(e->add_handler(std::move(h)));
        return e;
    }
    Json fw_manifest()
    {
        Json m = manifest();
        m["artifactFormat"] = "fw";
        return m;
    }
};

}  // namespace

TEST_F(OtaHandler, SemRebootConcluiEOEstadoDoHandlerAtravessaOReinicio)
{
    platform.wall = 1790590000000LL;
    deliver(fw_manifest());
    FakeHandler* h = nullptr;
    {
        auto e = engine_with(h);
        h->script = {InstallStep::pending(2000)};
        EXPECT_EQ(e->tick(), 2000);
        EXPECT_EQ(stage(), Stage::Installing);
        EXPECT_TRUE(fs::exists(paths.download + "/" + kId + ".fwbin"));
    }
    // O agente reiniciou no meio: só acompanha, não pede de novo.
    auto again = engine_with(h);
    h->script = {InstallStep::completed({{"slot", "mcu"}, {"mcuFw", "1.2.3"}})};
    again->tick();
    EXPECT_EQ(h->requests, 0) << "o `requested` persistido evita o segundo pedido";
    const auto ev = drain();
    EXPECT_EQ(codes(ev), (std::vector<std::string>{"ota-download-started", "ota-download-completed",
                                                    "ota-verified", "ota-install-started",
                                                    "ota-installed", "ota-completed"}));
    EXPECT_EQ(ev[5]["args"]["mcuFw"], "1.2.3");
    EXPECT_EQ(ev[5]["args"]["slot"], "mcu");
    EXPECT_EQ(platform.reboots, 0);
    EXPECT_FALSE(has_state());
    EXPECT_TRUE(fs::list(paths.download, "").empty());
}

TEST_F(OtaHandler, FalhaDoHandlerViraFailedNoInstall)
{
    platform.wall = 1790590000000LL;
    deliver(fw_manifest());
    FakeHandler* h = nullptr;
    auto e = engine_with(h);
    h->script = {InstallStep::failed("MCU_LOST", "o daemon esqueceu", true, {{"slot", "mcu"}})};
    e->tick();
    const auto ev = drain();
    ASSERT_EQ(ev.back()["code"], "ota-failed");
    EXPECT_EQ(ev.back()["args"]["stage"], "install");
    EXPECT_EQ(ev.back()["args"]["code"], "MCU_LOST");
    EXPECT_EQ(ev.back()["args"]["retryable"], true);
    EXPECT_EQ(ev.back()["args"]["slot"], "mcu");
    EXPECT_FALSE(has_state());
}

TEST_F(OtaHandler, AcceptRecusaAntesDeBaixar)
{
    platform.wall = 1790590000000LL;
    deliver(fw_manifest());
    FakeHandler* h = nullptr;
    auto e = engine_with(h);
    Failure f;
    f.stage = FailStage::Install;
    f.code = "FORMAT_UNSUPPORTED";
    f.message = "só o artifactName x";
    h->refuse_accept = f;
    e->tick();
    const auto ev = drain();
    ASSERT_EQ(codes(ev), (std::vector<std::string>{"ota-failed"}));
    EXPECT_EQ(ev[0]["args"]["code"], "FORMAT_UNSUPPORTED");
    EXPECT_TRUE(downloader.resumed_from.empty());
}

TEST_F(OtaHandler, CheckRecusaApagaEnaoInstala)
{
    platform.wall = 1790590000000LL;
    deliver(fw_manifest());
    FakeHandler* h = nullptr;
    auto e = engine_with(h);
    Failure f;
    f.stage = FailStage::Verify;
    f.code = "BUNDLE_INVALID";
    f.message = "não é o arquivo esperado";
    h->refuse_check = f;
    e->tick();
    const auto ev = drain();
    ASSERT_EQ(ev.back()["code"], "ota-failed");
    EXPECT_EQ(ev.back()["args"]["stage"], "verify");
    EXPECT_EQ(ev.back()["args"]["code"], "BUNDLE_INVALID");
    EXPECT_EQ(h->calls, 0);
    EXPECT_TRUE(fs::list(paths.download, "").empty());
}

TEST_F(OtaHandler, FormatoRepetidoNaoRegistra)
{
    auto e = engine();
    EXPECT_FALSE(e->add_handler(std::make_unique<RaucHandler>(installer)));
    EXPECT_FALSE(e->add_handler(nullptr));
}

/* ----------------------------------------------------- assinatura ed25519 --- */

namespace {

class OtaSigned : public OtaHandler {
protected:
    void SetUp() override
    {
        OtaHandler::SetUp();
        ASSERT_TRUE(key.valid());
        fs::make_dirs(paths.keyring);
        ASSERT_TRUE(key.write_public(paths.keyring + "/fw-release.pub"));
        platform.wall = 1790590000000LL;
    }
    Json signed_manifest(const test::TestKey& signer)
    {
        Json m = fw_manifest();
        m["signature"] = signer.sign_sha256(m["sha256"].get<std::string>());
        m["signingKeyId"] = "fw-release";
        return m;
    }
    test::TestKey key;
};

}  // namespace

TEST_F(OtaSigned, AssinaturaQueConferePassa)
{
    deliver(signed_manifest(key));
    FakeHandler* h = nullptr;
    auto e = engine_with(h);
    h->policy = ArtifactHandler::Signature::Required;
    h->script = {InstallStep::completed({{"slot", "mcu"}})};
    e->tick();
    EXPECT_EQ(codes(drain()).back(), "ota-completed");
}

TEST_F(OtaSigned, AssinadoPorOutraChaveNaoInstala)
{
    test::TestKey other;
    deliver(signed_manifest(other));
    FakeHandler* h = nullptr;
    auto e = engine_with(h);
    h->policy = ArtifactHandler::Signature::Required;
    e->tick();
    const auto ev = drain();
    ASSERT_EQ(ev.back()["code"], "ota-failed");
    EXPECT_EQ(ev.back()["args"]["stage"], "verify");
    EXPECT_EQ(ev.back()["args"]["code"], "SIGNATURE_INVALID");
    EXPECT_EQ(ev.back()["args"]["retryable"], false);
    EXPECT_EQ(h->calls, 0);
    EXPECT_TRUE(fs::list(paths.download, "").empty());
}

TEST_F(OtaSigned, ChaveForaDoChaveiroNaoInstala)
{
    Json m = signed_manifest(key);
    m["signingKeyId"] = "outra-chave";
    deliver(m);
    FakeHandler* h = nullptr;
    auto e = engine_with(h);
    h->policy = ArtifactHandler::Signature::Required;
    e->tick();
    const auto ev = drain();
    ASSERT_EQ(ev.back()["code"], "ota-failed");
    EXPECT_EQ(ev.back()["args"]["code"], "UNKNOWN_SIGNING_KEY");
    EXPECT_EQ(h->calls, 0);
}

TEST_F(OtaSigned, ObrigatoriaSemSignatureNemBaixa)
{
    deliver(fw_manifest());
    FakeHandler* h = nullptr;
    auto e = engine_with(h);
    h->policy = ArtifactHandler::Signature::Required;
    e->tick();
    const auto ev = drain();
    ASSERT_EQ(codes(ev), (std::vector<std::string>{"ota-failed"}));
    EXPECT_EQ(ev[0]["args"]["stage"], "verify");
    EXPECT_EQ(ev[0]["args"]["code"], "SIGNATURE_REQUIRED");
    EXPECT_TRUE(downloader.resumed_from.empty());
}

TEST_F(OtaSigned, SeHouverConfereQuandoVemENaoExigeQuandoNaoVem)
{
    test::TestKey other;
    deliver(signed_manifest(other));
    FakeHandler* h = nullptr;
    {
        auto e = engine_with(h);  // IfPresent
        e->tick();
    }
    auto ev = drain();
    EXPECT_EQ(ev.back()["args"]["code"], "SIGNATURE_INVALID");

    deliver(fw_manifest());  // sem signature: segue
    auto e = engine_with(h);
    h->script = {InstallStep::completed({{"slot", "mcu"}})};
    e->tick();
    EXPECT_EQ(codes(drain()).back(), "ota-completed");
}

TEST(OtaFalha, OsStagesSaoOsDoCatalogo)
{
    // Enum do `stage` do ota-failed (evento 49, migrations 0050/0107 do
    // dextrolabs-device).
    EXPECT_EQ(fail_stage_name(FailStage::Download), "download");
    EXPECT_EQ(fail_stage_name(FailStage::Verify), "verify");
    EXPECT_EQ(fail_stage_name(FailStage::Install), "install");
    EXPECT_EQ(fail_stage_name(FailStage::Reboot), "reboot");
    EXPECT_EQ(fail_stage_name(FailStage::MarkGood), "mark-good");
}
