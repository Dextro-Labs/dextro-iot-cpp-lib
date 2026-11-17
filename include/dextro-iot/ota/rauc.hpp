#pragma once

#include <dextro-iot/ota/artifact_handler.hpp>
#include <dextro-iot/ota/ports.hpp>

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

/// `rauc install <bundle>`, isolado para os testes trocarem por um falso.
class Installer {
public:
    virtual ~Installer() = default;
    /// `message` = o motivo, na falha. Não é interrompível: um `rauc install`
    /// pela metade é pior que um inteiro, e o trabalho de verdade é do
    /// `rauc.service`, não deste processo (`keep_going` só alimenta o
    /// watchdog).
    virtual bool install(const std::string& bundle, std::string& message,
                         const KeepGoing& keep_going) = 0;
};

/// O `rauc install` de verdade: cliente D-Bus do `rauc.service`, que abre o
/// bundle, confere a assinatura CMS contra o keyring da imagem, escreve o slot
/// inativo e o marca primário no bootloader.
class RaucInstaller final : public Installer {
public:
    /// `rauc` = o binário (no PATH, ou caminho absoluto).
    explicit RaucInstaller(std::string rauc = "rauc");
    bool install(const std::string& bundle, std::string& message,
                 const KeepGoing& keep_going) override;

private:
    std::string rauc_;
};

/// O handler do `raucb`.
///
///   verified ──▶ installing: slot alvo = o outro (`Platform::other_slot`),
///     `ota-install-started {slot}`, `rauc install` ──▶ `InstallStep::reboot`
///     (o `Engine` emite `ota-installed {slot}`, pede o reboot e confirma pelo
///     mark-good no boot novo; voltar ao slot antigo é `ota-rolled-back`).
///
/// A assinatura ed25519 do manifesto é `Ignored`: o bundle traz a dele (CMS),
/// conferida pelo `rauc install` contra o keyring da imagem — o contrato
/// (contracts.md A.4.3) dispensa `signature` no `raucb`.
///
/// Falhas: `install` `SLOT_UNKNOWN` (boot fora de um slot A/B; sem retry),
/// `RAUC_INSTALL_FAILED` (sem retry: assinatura, compatible ou o próprio
/// bundle — baixar de novo não muda).
class RaucHandler final : public ArtifactHandler {
public:
    explicit RaucHandler(Installer& installer);

    std::string format() const override { return "raucb"; }
    Signature signature() const override { return Signature::Ignored; }
    InstallStep install(InstallContext& ctx) override;

private:
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
