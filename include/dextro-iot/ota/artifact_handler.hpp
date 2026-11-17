#pragma once

#include <dextro-iot/ota/job.hpp>
#include <dextro-iot/ota/ports.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

/// O que o `Engine` não sabe: como se instala um formato de artefato.
///
/// O `Engine` faz a parte comum a todo formato — inbox, `applyAfter`,
/// download retomável, tamanho, sha256, assinatura ed25519, estado
/// persistido, trilha, reboot e confirmação. O `ArtifactHandler` do
/// `artifactFormat` do manifesto decide o resto:
///
///   - `accept()`, antes de baixar (ex.: só este `artifactName`);
///   - `check()`, depois do sha256 e da assinatura (ex.: a forma do arquivo);
///   - `install()`, chamado a cada volta enquanto o item está em `installing`.
///
/// A biblioteca traz o `RaucHandler` (`raucb`, `ota/rauc.hpp`). Outros
/// formatos são da aplicação — no controlador SSAT, os bundles `dxu` dos
/// módulos de potência e o `bin` do coprocessador, entregues a outro daemon.
namespace dextro::iot::ota {

/// O `stage` do `ota-failed` (enum do schema do evento 49 no catálogo do
/// tipo: `download|verify|install|reboot|mark-good`, migrations 0050/0107 do
/// dextrolabs-device).
enum class FailStage { Download, Verify, Install, Reboot, MarkGood };

std::string_view fail_stage_name(FailStage stage) noexcept;

/// Uma recusa, com o que vai no `ota-failed`.
class Failure {
public:
    FailStage stage = FailStage::Install;
    std::string code;  ///< `FORMAT_UNSUPPORTED`, `BUNDLE_INVALID`...
    std::string message;
    bool retryable = false;
    nlohmann::json extra = nlohmann::json::object();  ///< args a mais no evento
};

/// O resultado de uma volta do `install()`.
class InstallStep {
public:
    enum class Kind {
        Pending,    ///< ainda em curso: chame de novo em `wait_ms`
        Reboot,     ///< o slot alvo recebeu a imagem: `ota-installed`, e o
                    ///< `Engine` segue para o reboot e a confirmação (A/B)
        Completed,  ///< aplicado sem reboot do device: `ota-installed` +
                    ///< `ota-completed` com `extra` (que tem de trazer `slot`)
        Failed,     ///< `ota-failed` com `failure`
    };
    Kind kind = Kind::Pending;
    std::int64_t wait_ms = 0;
    nlohmann::json extra = nlohmann::json::object();
    Failure failure;

    static InstallStep pending(std::int64_t wait_ms);
    static InstallStep reboot(nlohmann::json extra = nlohmann::json::object());
    static InstallStep completed(nlohmann::json extra);
    static InstallStep failed(std::string code, std::string message, bool retryable,
                              nlohmann::json extra = nlohmann::json::object());
};

/// O que o `install()` recebe do `Engine`.
class InstallContext {
public:
    /// O item. O handler pode escrever `job.from_slot`/`job.target_slot` (o
    /// `Engine` os usa no reboot e na confirmação) e o seu `job.handler`.
    Job& job;
    /// O artefato já verificado (`<download>/<id>.<extensão>`).
    const std::string& file;
    Platform& platform;
    const KeepGoing& keep_going;
    /// Grava o estado do item agora (antes de um passo que não pode repetir).
    std::function<bool()> save;
    /// Um evento da trilha, com `campaignItemId` e `version` acrescentados
    /// (ex.: `ota-install-started {slot}`).
    std::function<void(std::string_view code, nlohmann::json args)> emit;
};

class ArtifactHandler {
public:
    /// Quanto o `Engine` exige da assinatura ed25519 do manifesto.
    enum class Signature {
        Ignored,    ///< o formato traz a própria (o bundle RAUC, CMS): não se confere
        IfPresent,  ///< confere quando o manifesto traz `signature`
        Required,   ///< sem `signature`/`signingKeyId`, `verify` `SIGNATURE_REQUIRED`
    };

    virtual ~ArtifactHandler() = default;

    /// O `artifactFormat` que este handler instala (`"raucb"`).
    virtual std::string format() const = 0;
    /// A extensão do arquivo baixado (padrão: o formato).
    virtual std::string extension() const { return format(); }
    virtual Signature signature() const { return Signature::IfPresent; }

    /// Antes de baixar. `nullopt` = segue.
    virtual std::optional<Failure> accept(const Manifest& manifest)
    {
        (void)manifest;
        return std::nullopt;
    }

    /// Depois do sha256 e da assinatura, com o arquivo inteiro. `nullopt` =
    /// verificado. O `Engine` apaga o arquivo numa recusa.
    virtual std::optional<Failure> check(const Manifest& manifest, const std::string& file)
    {
        (void)manifest;
        (void)file;
        return std::nullopt;
    }

    /// Uma volta da instalação. Pode bloquear (o `rauc install` leva minutos);
    /// chame `ctx.keep_going` no meio para o watchdog. Persistir o que não pode
    /// repetir em `ctx.job.handler` + `ctx.save()` ANTES de fazer.
    virtual InstallStep install(InstallContext& ctx) = 0;
};

}  // namespace dextro::iot::ota
