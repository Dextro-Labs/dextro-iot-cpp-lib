// As peças do módulo ota fora da máquina de estados: manifesto, estado,
// sha256, assinatura, `rauc status`, `systemctl show`, cmdline, o ambiente dos
// filhos e o que o CurlDownloader decide sem rede.
//
// Origem: tests/unit/manifest_test.cpp e system_test.cpp do ssat-ota-agent
// (origin/devel 76aa940); os de `rauc status`, `LinuxPlatform` e
// `CurlDownloader` são novos.
#include <dextro-iot/ota/curl_downloader.hpp>
#include <dextro-iot/ota/fs.hpp>
#include <dextro-iot/ota/job.hpp>
#include <dextro-iot/ota/linux_platform.hpp>
#include <dextro-iot/ota/manifest.hpp>
#include <dextro-iot/ota/rauc.hpp>
#include <dextro-iot/ota/sha256.hpp>
#include <dextro-iot/ota/signature.hpp>

#include "ota_test_keys.hpp"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>

#ifndef DEXTRO_IOT_TEST_DIR
#error "DEXTRO_IOT_TEST_DIR não definido"
#endif

namespace {

using namespace dextro::iot::ota;
using Json = nlohmann::json;

const std::string kId = "0190f7a2-6c1e-7b3a-9f00-1234567890ab";

Json valid()
{
    return {{"version", "20260927202307"},
            {"artifactUrl", "https://api.dextrolabs.com.br/v1/devices/artifact-download/tipo/img?version=1"},
            {"sha256", std::string(64, 'A')},
            {"sizeBytes", 71973626},
            {"artifactFormat", "raucb"},
            {"applyAfter", 1790590000000LL},
            {"campaignItemId", kId},
            {"progressEntityId", kId}};
}

std::string tmp(const char* name)
{
    return std::string(DEXTRO_IOT_TEST_DIR) + "/op-" + name + "-" + std::to_string(::getpid());
}

/// Um executável de mentira: `#!/bin/sh` com `body`.
std::string fake_tool(const char* name, const std::string& body)
{
    const std::string path = tmp(name);
    std::ofstream(path) << "#!/bin/sh\n" << body << "\n";
    ::chmod(path.c_str(), 0755);
    return path;
}

/// O formato de `rauc status --output-format=shell` (RAUC v1.13,
/// r_status_formatter_shell, valores com g_shell_quote), de um A/B com um
/// appfs filho.
const char* kRaucStatus =
    "RAUC_SYSTEM_COMPATIBLE='allprint-orangepi-3b'\n"
    "RAUC_SYSTEM_VARIANT=''\n"
    "RAUC_SYSTEM_BOOTED_BOOTNAME='A'\n"
    "RAUC_BOOT_PRIMARY='rootfs.0'\n"
    "RAUC_SYSTEM_SLOTS='rootfs.0 rootfs.1 appfs.0'\n"
    "RAUC_SLOTS='1 2 3'\n"
    "RAUC_SLOT_STATE_1='booted'\n"
    "RAUC_SLOT_CLASS_1='rootfs'\n"
    "RAUC_SLOT_DEVICE_1='/dev/mmcblk0p9'\n"
    "RAUC_SLOT_TYPE_1='ext4'\n"
    "RAUC_SLOT_BOOTNAME_1='A'\n"
    "RAUC_SLOT_PARENT_1=''\n"
    "RAUC_SLOT_MOUNTPOINT_1='/'\n"
    "RAUC_SLOT_BOOT_STATUS_1='good'\n"
    "RAUC_SLOT_STATE_2='inactive'\n"
    "RAUC_SLOT_CLASS_2='rootfs'\n"
    "RAUC_SLOT_DEVICE_2='/dev/mmcblk0p10'\n"
    "RAUC_SLOT_TYPE_2='ext4'\n"
    "RAUC_SLOT_BOOTNAME_2='B'\n"
    "RAUC_SLOT_PARENT_2=''\n"
    "RAUC_SLOT_MOUNTPOINT_2=''\n"
    "RAUC_SLOT_BOOT_STATUS_2='bad'\n"
    "RAUC_SLOT_STATE_3='inactive'\n"
    "RAUC_SLOT_CLASS_3='appfs'\n"
    "RAUC_SLOT_BOOTNAME_3=''\n"
    "RAUC_SLOT_PARENT_3='rootfs.0'\n"
    "RAUC_SLOT_BOOT_STATUS_3=''\n";

}  // namespace

TEST(OtaManifesto, OContratoEAceitoEOSha256FicaMinusculo)
{
    std::string why;
    const auto m = Manifest::parse(valid().dump(), why);
    ASSERT_TRUE(m) << why;
    EXPECT_EQ(m->campaign_item_id, kId);
    EXPECT_EQ(m->version, "20260927202307");
    EXPECT_EQ(m->sha256, std::string(64, 'a'));
    EXPECT_EQ(m->size_bytes, 71973626);
    EXPECT_EQ(m->apply_after_ms, 1790590000000LL);
    EXPECT_EQ(m->artifact_format, "raucb");
    const auto again = Manifest::parse(m->to_json().dump(), why);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->to_json(), m->to_json());
}

TEST(OtaManifesto, RecusasDizemOMotivoEDevolvemOIdQuandoEleEValido)
{
    std::string why;
    std::string id;
    Json j = valid();
    j["artifactUrl"] = "http://x/y";
    EXPECT_FALSE(Manifest::parse(j.dump(), why, &id));
    EXPECT_EQ(id, kId);

    j = valid();
    j["sha256"] = "abc";
    EXPECT_FALSE(Manifest::parse(j.dump(), why));
    j = valid();
    j["version"] = "1.0 beta";
    EXPECT_FALSE(Manifest::parse(j.dump(), why));
    j = valid();
    j["sizeBytes"] = -1;
    EXPECT_FALSE(Manifest::parse(j.dump(), why));

    id.clear();
    j = valid();
    j["campaignItemId"] = "../etc/passwd";
    EXPECT_FALSE(Manifest::parse(j.dump(), why, &id));
    EXPECT_TRUE(id.empty());
    EXPECT_FALSE(Manifest::parse("lixo", why));
}

TEST(OtaManifesto, EstadoDoItemIdaEVoltaComOEstadoDoHandler)
{
    std::string why;
    Job job;
    job.manifest = *Manifest::parse(valid().dump(), why);
    job.stage = Stage::Rebooting;
    job.attempt = 2;
    job.bytes = 10;
    job.from_slot = "A";
    job.target_slot = "B";
    job.install_boot_id = "boot-1";
    job.handler = {{"started", true}, {"trilha", "t-1"}};
    const auto back = Job::from_json(job.to_json());
    ASSERT_TRUE(back);
    EXPECT_EQ(back->stage, Stage::Rebooting);
    EXPECT_EQ(back->attempt, 2);
    EXPECT_EQ(back->target_slot, "B");
    EXPECT_EQ(back->install_boot_id, "boot-1");
    EXPECT_EQ(back->handler["trilha"], "t-1");
    EXPECT_FALSE(Job::from_json(Json{{"stage", "voando"}}));
    Json torto = job.to_json();
    torto["attempt"] = "dois";
    EXPECT_FALSE(Job::from_json(torto)) << "campo com tipo errado não pode lançar";
}

TEST(OtaSha256, ArquivoConhecido)
{
    const std::string path = tmp("sha");
    std::ofstream(path) << "abc";
    EXPECT_EQ(sha256_file(path),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_FALSE(sha256_file(path, [] { return false; }));  // interrompido
    fs::remove_file(path);
    EXPECT_FALSE(sha256_file(path));
}

TEST(OtaAssinatura, ConfereRecusaEDizPorque)
{
    const std::string dir = tmp("keys");
    fs::make_dirs(dir);
    test::TestKey key;
    ASSERT_TRUE(key.write_public(dir + "/k1.pub"));
    const std::string sha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const std::string sig = key.sign_sha256(sha);
    std::string why;
    EXPECT_EQ(signature::verify_sha256(dir, "k1", sha, sig, why), signature::Verdict::Valid);
    EXPECT_EQ(signature::verify_sha256(dir, "k1", std::string(64, '0'), sig, why),
              signature::Verdict::Invalid);
    EXPECT_EQ(signature::verify_sha256(dir, "k2", sha, sig, why), signature::Verdict::KeyUnknown);
    EXPECT_EQ(signature::verify_sha256(dir, "../k1", sha, sig, why),
              signature::Verdict::KeyUnknown);
    EXPECT_EQ(signature::verify_sha256(dir, "k1", sha, "curta", why),
              signature::Verdict::Malformed);
    std::ofstream(dir + "/lixo.pub") << "não é PEM";
    EXPECT_EQ(signature::verify_sha256(dir, "lixo", sha, sig, why),
              signature::Verdict::KeyInvalid);
    fs::remove_file(dir + "/k1.pub");
    fs::remove_file(dir + "/lixo.pub");
    ::rmdir(dir.c_str());
}

TEST(OtaRauc, StatusShellComAspasDoGlib)
{
    RaucStatus s;
    ASSERT_TRUE(parse_rauc_status(kRaucStatus, s));
    EXPECT_EQ(s.compatible, "allprint-orangepi-3b");
    EXPECT_EQ(s.booted, "A");
    EXPECT_EQ(s.primary, "rootfs.0");
    ASSERT_EQ(s.slots.size(), 3U);
    EXPECT_EQ(s.slots[1].name, "rootfs.1");
    EXPECT_EQ(s.slots[1].bootname, "B");
    EXPECT_EQ(s.slots[1].boot_status, "bad");
    EXPECT_EQ(s.slots[2].cls, "appfs");
    EXPECT_EQ(s.other_bootname("A"), "B");
    EXPECT_EQ(s.other_bootname("b"), "A");
    ASSERT_NE(s.by_bootname("b"), nullptr);
    EXPECT_EQ(s.by_bootname("b")->name, "rootfs.1");
    EXPECT_EQ(s.by_bootname(""), nullptr);
    EXPECT_FALSE(parse_rauc_status("RAUC_SYSTEM_COMPATIBLE='x'\n", s));
}

TEST(OtaRauc, TresSlotsNaoSaoUmParAB)
{
    RaucStatus s;
    s.slots = {{"r.0", "rootfs", "booted", "A", "good"},
               {"r.1", "rootfs", "inactive", "B", "good"},
               {"r.2", "rootfs", "inactive", "C", "good"}};
    EXPECT_EQ(s.other_bootname("A"), "");
}

TEST(OtaRauc, ShellUnquote)
{
    EXPECT_EQ(shell_unquote("'a b'"), "a b");
    EXPECT_EQ(shell_unquote("'it'\\''s'"), "it's");
    EXPECT_EQ(shell_unquote("\"x\\\"y\""), "x\"y");
    EXPECT_EQ(shell_unquote("semaspas"), "semaspas");
    EXPECT_EQ(shell_unquote("''"), "");
}

TEST(OtaPlataforma, SlotDaCmdlineEMarkGood)
{
    EXPECT_EQ(slot_from_cmdline("root=PARTUUID=x ssat.slot=b ssat.boot=zimage", "ssat.slot="), "b");
    EXPECT_EQ(slot_from_cmdline("root=/dev/mmcblk0p9 rauc.slot=A", "rauc.slot="), "A");
    EXPECT_EQ(slot_from_cmdline("root=/dev/ram0 ssat.boot=recovery", "rauc.slot="), "");
    EXPECT_EQ(slot_from_cmdline("rauc.slot=A", ""), "");
    EXPECT_EQ(parse_mark_good("ActiveState=inactive\nResult=success\nExecMainStatus=0\n"
                              "ExecMainExitTimestampMonotonic=123456\n"),
              MarkGood::Good);
    EXPECT_EQ(parse_mark_good("ActiveState=active\nResult=success\nExecMainStatus=0\n"
                              "ExecMainExitTimestampMonotonic=0\n"),
              MarkGood::Pending);
    // Nem rodou neste boot.
    EXPECT_EQ(parse_mark_good("ActiveState=inactive\nResult=success\nExecMainStatus=0\n"
                              "ExecMainExitTimestampMonotonic=0\n"),
              MarkGood::Pending);
    EXPECT_EQ(parse_mark_good("ActiveState=failed\nResult=exit-code\nExecMainStatus=1\n"
                              "ExecMainExitTimestampMonotonic=99\n"),
              MarkGood::Failed);
}

TEST(OtaPlataforma, LinuxPlatformLeORaucStatusEOSystemctl)
{
    const std::string status_file = tmp("rauc-status");
    std::ofstream(status_file) << kRaucStatus;
    // `rauc status --output-format=shell` → o texto acima; o resto falha.
    const std::string rauc = fake_tool(
        "rauc", "[ \"$1\" = status ] && [ \"$2\" = --output-format=shell ] && cat '" +
                    status_file + "' && exit 0\nexit 1");
    const std::string systemctl = fake_tool(
        "systemctl",
        "[ \"$1\" = show ] && [ \"${10}\" = minha-mark-good.service ] && "
        "printf 'ActiveState=inactive\\nResult=success\\nExecMainStatus=0\\n"
        "ExecMainExitTimestampMonotonic=5\\n' && exit 0\nexit 1");

    LinuxPlatform::Config c;
    c.slot_cmdline_key = "dextro.chave-que-nao-existe=";  // cai no rauc status
    c.rauc = rauc;
    c.systemctl = systemctl;
    c.mark_good_unit = "minha-mark-good.service";
    LinuxPlatform p(c);
    EXPECT_EQ(p.current_slot(), "A");
    EXPECT_EQ(p.other_slot("A"), "B");
    EXPECT_TRUE(p.slot_bad("B"));
    EXPECT_FALSE(p.slot_bad("A"));
    EXPECT_EQ(p.mark_good(), MarkGood::Good);
    EXPECT_FALSE(p.boot_id().empty());

    c.rauc = "/caminho/que/nao/existe/rauc";
    LinuxPlatform sem_rauc(c);
    EXPECT_EQ(sem_rauc.other_slot("A"), "");
    EXPECT_FALSE(sem_rauc.slot_bad("B"));

    for (const std::string& f : {status_file, rauc, systemctl}) {
        fs::remove_file(f);
    }
}

TEST(OtaPlataforma, RaucInstallerDizOMotivoDaUltimaLinha)
{
    const std::string rauc = fake_tool(
        "rauc-install", "echo 'installing'\necho 'LastError: signature verification failed' >&2\nexit 1");
    RaucInstaller installer(rauc);
    std::string message;
    EXPECT_FALSE(installer.install("/tmp/x.raucb", message, {}));
    EXPECT_NE(message.find("saiu com 1"), std::string::npos);
    EXPECT_NE(message.find("signature verification failed"), std::string::npos);
    fs::remove_file(rauc);
}

TEST(OtaPlataforma, FilhoNaoHerdaOSocketDeNotificacaoDoSystemd)
{
    // Sob o systemd o agente tem NOTIFY_SOCKET; o rauc/systemctl que ele chama
    // não pode herdar, ou o sd_notify deles cai no socket do agente.
    ::setenv("NOTIFY_SOCKET", "/run/systemd/notify", 1);
    ::setenv("WATCHDOG_USEC", "60000000", 1);
    ::setenv("DEXTRO_OTA_TESTE", "fica", 1);

    std::string out;
    const int rc = run_command(
        {"sh", "-c", "echo \"${NOTIFY_SOCKET:-vazio} ${WATCHDOG_USEC:-vazio} ${DEXTRO_OTA_TESTE:-vazio}\""},
        out, [] { return true; });
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(out, "vazio vazio fica\n");

    ::unsetenv("NOTIFY_SOCKET");
    ::unsetenv("WATCHDOG_USEC");
    ::unsetenv("DEXTRO_OTA_TESTE");
    EXPECT_EQ(run_command({"/caminho/que/nao/existe"}, out), -1);
}

TEST(OtaDownload, SoHttpsENaoTocaNaRede)
{
    CurlDownloader d;
    DownloadRequest r;
    r.url = "http://artifacts.dextrolabs.com.br/x";
    r.path = tmp("part-http");
    r.token = "dxd_x";
    const DownloadResult res = d.fetch(r, {});
    EXPECT_EQ(res.outcome, DownloadResult::Outcome::Permanent);
    EXPECT_FALSE(res.retryable);
    EXPECT_FALSE(fs::exists(r.path));
}

TEST(OtaDownload, PartJaInteiroNaoBaixaDeNovo)
{
    // Caiu entre o fim do download e o rename: o `.part` já tem tudo.
    CurlDownloader d;
    DownloadRequest r;
    r.url = "https://nao-resolve.invalid/x";
    r.path = tmp("part-inteiro");
    std::ofstream(r.path) << "0123456789";
    r.expected_size = 10;
    const DownloadResult res = d.fetch(r, {});
    EXPECT_EQ(res.outcome, DownloadResult::Outcome::Ok);
    EXPECT_EQ(res.total_bytes, 10);
    fs::remove_file(r.path);
}
