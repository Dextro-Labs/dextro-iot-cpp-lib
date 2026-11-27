#pragma once

#include <dextro-iot/ota/artifact_handler.hpp>
#include <dextro-iot/ota/ports.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/// O formato `raucb`: a imagem inteira, por RAUC, num sistema A/B.
///
/// Referência: o manual do RAUC (https://rauc.readthedocs.io/, seções
/// "Integration" — `rauc.slot=` na cmdline, `rauc-mark-good.service` — e
/// "Reference" — `rauc status --output-format=shell`) e o código do RAUC
/// v1.13 (`src/main.c`, `r_status_formatter_shell`, valores com
/// `g_shell_quote`).
///
/// Origem: `RaucInstaller` e o ramo `raucb` do `Engine` do ssat-ota-agent
/// (`origin/devel` 76aa940).
namespace dextro::iot::ota {

/// O `rauc.service` agora: as propriedades `Operation` e `LastError` da
/// interface D-Bus `de.pengutronix.rauc.Installer` (manual do RAUC,
/// "Reference" › "D-Bus API"; `src/de.pengutronix.rauc.Installer.xml`).
class RaucDaemon {
public:
    enum class Operation {
        Idle,     ///< `"idle"`: nenhuma instalação em curso
        Busy,     ///< qualquer outro valor (`"installing"`)
        Unknown,  ///< não deu para perguntar (sem D-Bus, sem o serviço)
    };
    Operation operation = Operation::Unknown;
    /// A última falha do daemon desde que ele subiu ("" = nenhuma). Não é
    /// zerada por uma instalação boa: só vale a que mudou durante a nossa.
    std::string last_error;
};

/// O registro de um slot no `rauc status --detailed --output-format=json`
/// (`slot_status`, gravado pelo daemon no fim da escrita: `src/install.c`,
/// `update_slot_status`, com o status `"pending"` durante a cópia).
class RaucSlotRecord {
public:
    std::string status;  ///< `"ok"` quando a escrita do slot terminou bem
    std::string build;   ///< `bundle.build` do que está no slot
    /// `installed.timestamp` em epoch ms (UTC), ou -1 sem registro.
    std::int64_t installed_ms = -1;
    /// O slot é o `boot_primary`: o bootloader sobe nele no próximo boot.
    bool primary = false;
};

/// Lê a saída de `busctl get-property ... Operation LastError` (uma linha
/// `s "valor"` por propriedade, na ordem pedida; busctl(1)).
RaucDaemon parse_rauc_daemon(const std::string& busctl_output);

/// O registro do slot com este `bootname` (sem caixa) na saída de `rauc
/// status --detailed --output-format=json`, ou vazio (JSON ilegível, slot
/// que não existe ou sem `slot_status`).
std::optional<RaucSlotRecord> parse_rauc_slot(const std::string& json,
                                              const std::string& bootname);

/// O RAUC visto pelo `RaucHandler`, isolado para os testes trocarem por um
/// falso.
class Installer {
public:
    virtual ~Installer() = default;
    /// `rauc install <bundle>`. `message` = o motivo, na falha. Não é
    /// interrompível: um `rauc install` pela metade é pior que um inteiro, e
    /// o trabalho de verdade é do `rauc.service`, não deste processo
    /// (`keep_going` só alimenta o watchdog). A saída do cliente NÃO é o
    /// resultado da instalação: o cliente pode cair (ou ser recusado) com o
    /// daemon escrevendo; quem decide é `daemon()` + `slot()`.
    virtual bool install(const std::string& bundle, std::string& message,
                         const KeepGoing& keep_going) = 0;
    /// O estado do daemon agora.
    virtual RaucDaemon daemon() = 0;
    /// O registro do slot de `bootname`, ou vazio. Com o daemon ocupado o
    /// RAUC recusa o `GetSlotStatus` (`src/service.c`): pergunte só com ele
    /// `Idle`.
    virtual std::optional<RaucSlotRecord> slot(const std::string& bootname) = 0;
};

/// O RAUC de verdade: o cliente `rauc` (D-Bus do `rauc.service`, que abre o
/// bundle, confere a assinatura CMS contra o keyring da imagem, escreve o
/// slot inativo e o marca primário no bootloader) e o `busctl` para as
/// propriedades do daemon.
class RaucInstaller final : public Installer {
public:
    /// `rauc`/`busctl` = os binários (no PATH, ou caminho absoluto).
    explicit RaucInstaller(std::string rauc = "rauc", std::string busctl = "busctl");
    bool install(const std::string& bundle, std::string& message,
                 const KeepGoing& keep_going) override;
    RaucDaemon daemon() override;
    std::optional<RaucSlotRecord> slot(const std::string& bootname) override;

private:
    std::string rauc_;
    std::string busctl_;
};

/// O handler do `raucb`.
///
///   verified ──▶ installing: slot alvo = o outro (`Platform::other_slot`),
///     `ota-install-started {slot}`, `rauc install` ──▶ `InstallStep::reboot`
///     (o `Engine` emite `ota-installed {slot}`, pede o reboot e confirma pelo
///     mark-good no boot novo; voltar ao slot antigo é `ota-rolled-back`).
///
/// O resultado é o do DAEMON, não o da saída do `rauc install`:
///   - daemon ocupado (o agente caiu no meio e voltou com o `rauc install`
///     de antes ainda escrevendo) → espera, sem mandar outro (o RAUC
///     recusaria com "Already processing a different method" e o cliente
///     sairia com 2, "D-Bus error while installing" — OTA-00095, 26/11/2026);
///   - daemon livre e o slot alvo com `status=ok`, primário e
///     `installed.timestamp` depois do `ota-install-started` → instalado,
///     mesmo que o cliente tenha saído com erro;
///   - senão, com `LastError` novo → `RAUC_INSTALL_FAILED` sem retry
///     (assinatura, compatible, o próprio bundle: baixar de novo não muda);
///   - sem motivo do daemon, mais uma vez; na segunda → `RAUC_INSTALL_FAILED`
///     com retry (foi o transporte, não o bundle).
///
/// A assinatura ed25519 do manifesto é `Ignored`: o bundle traz a dele (CMS),
/// conferida pelo `rauc install` contra o keyring da imagem — o contrato
/// (contracts.md A.4.3) dispensa `signature` no `raucb`.
///
/// Falhas: `install` `SLOT_UNKNOWN` (boot fora de um slot A/B; sem retry) e
/// `RAUC_INSTALL_FAILED` (acima).
class RaucHandler final : public ArtifactHandler {
public:
    explicit RaucHandler(Installer& installer);

    std::string format() const override { return "raucb"; }
    Signature signature() const override { return Signature::Ignored; }
    InstallStep install(InstallContext& ctx) override;

private:
    /// O alvo e o `ota-install-started`, uma vez por item. Devolve a recusa
    /// (boot fora de um slot A/B), ou vazio para seguir.
    std::optional<InstallStep> begin(InstallContext& ctx);
    /// O daemon já pôs a imagem no slot alvo, nesta instalação?
    bool written(const Job& job);
    /// Depois de um `rauc install`: instalado, falha, ou vazio = mais uma.
    std::optional<InstallStep> judge(const Job& job, const RaucDaemon& daemon);

    Installer& installer_;
};

/// Um slot do `rauc status --output-format=shell`.
class RaucSlot {
public:
    std::string name;         ///< `rootfs.0` (RAUC_SYSTEM_SLOTS)
    std::string cls;          ///< RAUC_SLOT_CLASS_n
    std::string state;        ///< RAUC_SLOT_STATE_n: booted | active | inactive
    std::string bootname;     ///< RAUC_SLOT_BOOTNAME_n (vazio: fora do bootloader)
    std::string boot_status;  ///< RAUC_SLOT_BOOT_STATUS_n: good | bad | vazio
};

class RaucStatus {
public:
    std::string compatible;     ///< RAUC_SYSTEM_COMPATIBLE
    std::string booted;         ///< RAUC_SYSTEM_BOOTED_BOOTNAME
    std::string primary;        ///< RAUC_BOOT_PRIMARY (nome do slot)
    std::vector<RaucSlot> slots;

    /// O slot com este `bootname` (comparação sem caixa), ou nulo.
    const RaucSlot* by_bootname(const std::string& bootname) const noexcept;
    /// O único outro `bootname` do sistema (o par A/B), ou vazio quando não há
    /// exatamente um.
    std::string other_bootname(const std::string& bootname) const;
};

/// Lê a saída de `rauc status --output-format=shell` (`CHAVE='valor'` por
/// linha, com as aspas do `g_shell_quote`). `false` se não há
/// `RAUC_SYSTEM_SLOTS`.
bool parse_rauc_status(const std::string& text, RaucStatus& out);

/// Desfaz o `g_shell_quote` (`'a'\''b'` → `a'b`) e aspas simples/duplas
/// simples. Texto sem aspas sai como está.
std::string shell_unquote(const std::string& text);

}  // namespace dextro::iot::ota
