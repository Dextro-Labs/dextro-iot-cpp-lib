#pragma once

#include <dextro-iot/ota/artifact_handler.hpp>
#include <dextro-iot/ota/event_sink.hpp>
#include <dextro-iot/ota/job.hpp>
#include <dextro-iot/ota/ports.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/// A máquina de estados do OTA (contrato da fase 1 do dextrolabs-device,
/// eventos `ota-*` 40..49 do catálogo do tipo).
///
///     inbox/<id>.json ──▶ received ──(applyAfter)──▶ downloading ──▶ downloaded
///        ──(tamanho + sha256 + ed25519 + check do handler)──▶ verified ──▶ installing
///        ├─ formato com reboot (raucb) ──▶ installed ──▶ rebooting ──(boot novo)
///        │                                   ──▶ confirming ──▶ completed | rolled-back
///        └─ formato sem reboot ──▶ completed | failed
///
/// Cada passo grava o estado (`state/<id>.json`) e emite o evento no
/// `EventSink`. Falha em qualquer passo vira `ota-failed` com `stage` (`download`,
/// `verify`, `install`, `reboot`, `mark-good`), `code`, `message` e `retryable`.
/// No fim (qualquer resultado), o manifesto sai do inbox e o estado e os
/// arquivos são apagados.
///
/// Idempotente por `campaignItemId`: o mesmo manifesto entregue de novo
/// durante o item não abre outro; entregue DEPOIS do fim (o retry do backend,
/// com o mesmo id), abre uma tentativa nova. Um item por vez, o mais adiantado
/// primeiro.
///
/// ## A `Engine` BLOQUEIA
///
/// `tick()` faz o download (minutos, num 4G), o sha256 de centenas de MB e o
/// `rauc install` sem devolver o controle. O README da biblioteca pede "uma
/// thread, um laço" para o `Client`: a `Engine` **não** pode rodar no laço do
/// `Client` — a sessão MQTT, o keepalive e as device-procedures parariam
/// junto. Rode-a:
///
///   - numa thread própria, com o seu laço (`tick()`, dormir o que ele
///     devolve), conversando com o laço do `Client` só por arquivos (inbox,
///     via `InboxOtaSpool`) e pelo `EventSink` (que então tem de ser seguro
///     para as duas threads); ou
///   - num processo próprio (como o `ssat-otad` do controlador SSAT: quem
///     atualiza não é quem está sendo atualizado), com o `InboxOtaSpool` do
///     processo do `Client` escrevendo no inbox dele.
///
/// Nada aqui é thread-safe: um `Engine`, uma thread.
///
/// Origem: `src/ota/engine.cpp` do ssat-ota-agent (`origin/devel` 76aa940). A
/// mudança é estrutural: os formatos `dxu`/`bin` do SSAT, fixos na máquina,
/// viraram `ArtifactHandler`; a pasta `outbox` virou o `EventSink`; os
/// caminhos `/var/lib/ssat/ota/...` viraram `Paths`.
namespace dextro::iot::ota {

class Engine {
public:
    class Paths {
    public:
        /// Onde o `InboxOtaSpool` deixa os manifestos.
        std::string inbox;
        /// O estado de cada item (fora dos slots A/B: atravessa o reboot).
        std::string state;
        /// Os `.part` e os artefatos inteiros (mesma partição de dados).
        std::string download;
        /// As chaves públicas ed25519 que a imagem confia, `<signingKeyId>.pub`.
        std::string keyring;

        /// `<base>/inbox`, `<base>/state`, `<base>/download` e o chaveiro dado
        /// (que normalmente vem da imagem, em `/etc`).
        static Paths under(const std::string& base, std::string keyring);

        bool valid() const noexcept;
    };

    /// Tentativas de download dentro de um item antes do `ota-failed`.
    static constexpr int kMaxDownloadAttempts = 5;
    /// Folga exigida além do que falta baixar.
    static constexpr std::int64_t kSpaceMargin = 16LL * 1024 * 1024;
    static constexpr int kMaxRebootTries = 3;
    static constexpr std::int64_t kRebootRetryMs = 60000;
    /// Confirmação: quanto esperar o mark-good neste boot, e quanto esperar a
    /// placa reiniciar sozinha depois de o mark-good falhar.
    static constexpr std::int64_t kConfirmTimeoutMs = 30LL * 60 * 1000;
    static constexpr std::int64_t kMarkGoodFailedGraceMs = 10LL * 60 * 1000;

    Engine(Paths paths, Downloader& downloader, Platform& platform, EventSink& events);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// Registra o handler de um formato. `false` (e nada registrado) com
    /// formato vazio ou já registrado.
    bool add_handler(std::unique_ptr<ArtifactHandler> handler);

    /// Chamado durante as operações longas (watchdog; SIGTERM).
    void set_keep_going(KeepGoing keep_going) { keep_going_ = std::move(keep_going); }

    /// Uma volta: lê o inbox, avança os itens. Pode BLOQUEAR por minutos.
    /// Devolve em quantos ms quer a próxima volta.
    std::int64_t tick();

    /// Espera entre tentativas de download (a `n`-ésima, n >= 1): 30 s · 2ⁿ⁻¹,
    /// teto de 10 min.
    static std::int64_t backoff_ms(int attempt) noexcept;

    const Paths& paths() const noexcept { return paths_; }

private:
    std::int64_t advance(Job& job);
    void adopt_inbox();
    bool save(const Job& job);
    void emit(const Job& job, std::string_view code,
              nlohmann::json extra = nlohmann::json::object());
    void fail(Job& job, FailStage stage, std::string_view code, std::string_view message,
              bool retryable, nlohmann::json extra = nlohmann::json::object());
    void fail(Job& job, const Failure& failure);
    void finish(Job& job, std::string_view result);
    void cleanup(const Job& job);
    std::string state_path(const std::string& id) const;
    std::string bundle_path(const Job& job) const;
    std::string part_path(const Job& job) const;
    ArtifactHandler* handler_for(const Job& job) const noexcept;
    bool keep_going() const { return !keep_going_ || keep_going_(); }
    bool verify_signature(Job& job, const std::string& sha);

    std::int64_t step_received(Job& job);
    std::int64_t step_downloading(Job& job);
    std::int64_t step_downloaded(Job& job);
    std::int64_t step_verified(Job& job);
    std::int64_t step_installing(Job& job);
    std::int64_t step_installed(Job& job);
    std::int64_t step_rebooting(Job& job);
    std::int64_t step_confirming(Job& job);

    Paths paths_;
    Downloader& downloader_;
    Platform& platform_;
    EventSink& events_;
    std::vector<std::unique_ptr<ArtifactHandler>> handlers_;
    KeepGoing keep_going_;

    /// Relógios deste boot (não persistidos: recomeçam com o processo).
    std::int64_t confirm_since_ms_ = -1;
    std::int64_t mark_good_failed_since_ms_ = -1;
};

}  // namespace dextro::iot::ota
