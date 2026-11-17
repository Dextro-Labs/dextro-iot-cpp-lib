#include <dextro-iot/ota/engine.hpp>

#include <dextro-iot/log.hpp>
#include <dextro-iot/ota/fs.hpp>
#include <dextro-iot/ota/sha256.hpp>
#include <dextro-iot/ota/signature.hpp>
#include <dextro-iot/procedures/procedure.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace dextro::iot::ota {
namespace {

using Json = nlohmann::json;

constexpr std::int64_t kIdleMs = 5000;
/// O manifesto e o estado são pequenos; maior que isto não é nosso.
constexpr std::size_t kMaxSmallFile = 64 * 1024;

std::string clip(std::string_view text, std::size_t max = 300)
{
    std::string s(text.substr(0, max));
    std::replace(s.begin(), s.end(), '\n', ' ');
    return s;
}

bool same_slot(const std::string& a, const std::string& b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

}  // namespace

std::string_view fail_stage_name(FailStage stage) noexcept
{
    switch (stage) {
    case FailStage::Download:
        return "download";
    case FailStage::Verify:
        return "verify";
    case FailStage::Install:
        return "install";
    case FailStage::Reboot:
        return "reboot";
    case FailStage::MarkGood:
        return "mark-good";
    }
    return "install";
}

InstallStep InstallStep::pending(std::int64_t wait_ms)
{
    InstallStep s;
    s.kind = Kind::Pending;
    s.wait_ms = wait_ms;
    return s;
}

InstallStep InstallStep::reboot(Json extra)
{
    InstallStep s;
    s.kind = Kind::Reboot;
    s.extra = std::move(extra);
    return s;
}

InstallStep InstallStep::completed(Json extra)
{
    InstallStep s;
    s.kind = Kind::Completed;
    s.extra = std::move(extra);
    return s;
}

InstallStep InstallStep::failed(std::string code, std::string message, bool retryable, Json extra)
{
    InstallStep s;
    s.kind = Kind::Failed;
    s.failure.stage = FailStage::Install;
    s.failure.code = std::move(code);
    s.failure.message = std::move(message);
    s.failure.retryable = retryable;
    s.failure.extra = std::move(extra);
    return s;
}

Engine::Paths Engine::Paths::under(const std::string& base, std::string keyring)
{
    Paths p;
    p.inbox = base + "/inbox";
    p.state = base + "/state";
    p.download = base + "/download";
    p.keyring = std::move(keyring);
    return p;
}

bool Engine::Paths::valid() const noexcept
{
    return !inbox.empty() && !state.empty() && !download.empty();
}

Engine::Engine(Paths paths, Downloader& downloader, Platform& platform, EventSink& events)
    : paths_(std::move(paths)), downloader_(downloader), platform_(platform), events_(events)
{
}

Engine::~Engine() = default;

bool Engine::add_handler(std::unique_ptr<ArtifactHandler> handler)
{
    if (!handler || handler->format().empty()) {
        return false;
    }
    const std::string format = handler->format();
    for (const auto& h : handlers_) {
        if (h->format() == format) {
            return false;
        }
    }
    handlers_.push_back(std::move(handler));
    return true;
}

ArtifactHandler* Engine::handler_for(const Job& job) const noexcept
{
    for (const auto& h : handlers_) {
        if (h->format() == job.manifest.artifact_format) {
            return h.get();
        }
    }
    return nullptr;
}

std::int64_t Engine::backoff_ms(int attempt) noexcept
{
    const int n = std::clamp(attempt, 1, 6);
    return std::min<std::int64_t>(30000LL << (n - 1), 10LL * 60 * 1000);
}

std::string Engine::state_path(const std::string& id) const
{
    return paths_.state + "/" + id + ".json";
}

std::string Engine::bundle_path(const Job& job) const
{
    const ArtifactHandler* h = handler_for(job);
    const std::string ext = h != nullptr ? h->extension() : std::string("bin");
    return paths_.download + "/" + job.manifest.campaign_item_id + "." + ext;
}

std::string Engine::part_path(const Job& job) const
{
    return bundle_path(job) + ".part";
}

bool Engine::save(const Job& job)
{
    if (!fs::write_atomic(state_path(job.manifest.campaign_item_id), job.to_json().dump())) {
        DEXTRO_IOT_ERR("ota %s: não gravou o estado (%s)", job.manifest.campaign_item_id.c_str(),
                       std::string(stage_name(job.stage)).c_str());
        return false;
    }
    return true;
}

void Engine::emit(const Job& job, std::string_view code, Json extra)
{
    Json args = extra.is_object() ? std::move(extra) : Json::object();
    args["campaignItemId"] = job.manifest.campaign_item_id;
    args["version"] = job.manifest.version;
    const std::string c(code);
    if (!events_.emit(code, args, platform_.wall_ms())) {
        DEXTRO_IOT_ERR("ota %s: evento %s não gravado", job.manifest.campaign_item_id.c_str(),
                       c.c_str());
        return;
    }
    DEXTRO_IOT_INFO("ota %s: %s %s", job.manifest.campaign_item_id.c_str(), c.c_str(),
                    args.dump().c_str());
}

void Engine::fail(Job& job, FailStage stage, std::string_view code, std::string_view message,
                  bool retryable, Json extra)
{
    Json args = extra.is_object() ? std::move(extra) : Json::object();
    args["stage"] = std::string(fail_stage_name(stage));
    args["code"] = std::string(code);
    args["message"] = clip(message);
    args["retryable"] = retryable;
    emit(job, "ota-failed", std::move(args));
    finish(job, "failed");
}

void Engine::fail(Job& job, const Failure& failure)
{
    fail(job, failure.stage, failure.code.empty() ? "FAILED" : failure.code, failure.message,
         failure.retryable, failure.extra);
}

void Engine::finish(Job& job, std::string_view result)
{
    job.stage = Stage::Done;
    job.result = std::string(result);
    save(job);
    cleanup(job);
}

/// O inbox primeiro, o estado por último: uma queda no meio deixa um estado
/// `done` sem manifesto, que a próxima volta só apaga.
void Engine::cleanup(const Job& job)
{
    const std::string& id = job.manifest.campaign_item_id;
    fs::remove_file(paths_.inbox + "/" + id + ".json");
    fs::remove_file(part_path(job));
    fs::remove_file(bundle_path(job));
    fs::remove_file(state_path(id));
    confirm_since_ms_ = -1;
    mark_good_failed_since_ms_ = -1;
    DEXTRO_IOT_INFO("ota %s: fim (%s)", id.c_str(), job.result.c_str());
}

void Engine::adopt_inbox()
{
    for (const std::string& name : fs::list(paths_.inbox, ".json")) {
        const std::string path = paths_.inbox + "/" + name;
        std::string text;
        if (!fs::read_file(path, text, kMaxSmallFile)) {
            continue;
        }
        std::string why;
        std::string id;
        auto manifest = Manifest::parse(text, why, &id);
        if (!manifest) {
            DEXTRO_IOT_WARN("ota: manifesto %s recusado: %s", path.c_str(), why.c_str());
            if (!id.empty()) {
                Job job;
                job.manifest.campaign_item_id = id;
                job.manifest.version = "?";
                // version pode ser justamente o que está errado: a trilha
                // leva o que houver, e o backend casa pelo campaignItemId.
                const Json j = Json::parse(text, nullptr, false);
                if (j.is_object() && j.contains("version") && j["version"].is_string() &&
                    procedures::is_version_token(j["version"].get<std::string>())) {
                    job.manifest.version = j["version"].get<std::string>();
                }
                emit(job, "ota-failed",
                     {{"stage", "download"},
                      {"code", "MANIFEST_INVALID"},
                      {"message", clip(why)},
                      {"retryable", false}});
            }
            if (std::rename(path.c_str(), (path + ".rejected").c_str()) != 0) {
                fs::remove_file(path);
            }
            continue;
        }
        if (fs::exists(state_path(manifest->campaign_item_id))) {
            continue;  // o mesmo item, entregue de novo: já está em andamento
        }
        Job job;
        job.manifest = std::move(*manifest);
        job.stage = Stage::Received;
        if (save(job)) {
            DEXTRO_IOT_INFO("ota %s: manifesto aceito (versão %s, %s, applyAfter %lld)",
                            job.manifest.campaign_item_id.c_str(), job.manifest.version.c_str(),
                            job.manifest.artifact_format.c_str(),
                            static_cast<long long>(job.manifest.apply_after_ms));
        }
    }
}

std::int64_t Engine::tick()
{
    if (!paths_.valid()) {
        DEXTRO_IOT_ERR("ota: Engine::Paths sem inbox/state/download");
        return kIdleMs;
    }
    fs::make_dirs(paths_.state);
    fs::make_dirs(paths_.download);
    adopt_inbox();

    std::vector<Job> jobs;
    for (const std::string& name : fs::list(paths_.state, ".json")) {
        std::string text;
        if (!fs::read_file(paths_.state + "/" + name, text, kMaxSmallFile)) {
            continue;
        }
        auto job = Job::from_json(Json::parse(text, nullptr, false));
        if (!job) {
            DEXTRO_IOT_ERR("ota: estado %s ilegível; descartado", name.c_str());
            fs::remove_file(paths_.state + "/" + name);
            continue;
        }
        jobs.push_back(std::move(*job));
    }
    // Um item por vez, o mais adiantado primeiro: duas instalações não cabem
    // num device, e um item que já instalou tem de ser confirmado antes de
    // qualquer outro começar.
    std::stable_sort(jobs.begin(), jobs.end(),
                     [](const Job& a, const Job& b) { return a.stage > b.stage; });

    std::int64_t wait = kIdleMs;
    for (Job& job : jobs) {
        if (job.stage == Stage::Done) {
            cleanup(job);
            continue;
        }
        wait = std::min(wait, advance(job));
        if (job.stage != Stage::Done && job.stage > Stage::Received) {
            break;  // este está em curso: os outros esperam
        }
    }
    return std::max<std::int64_t>(wait, 100);
}

std::int64_t Engine::advance(Job& job)
{
    for (int guard = 0; guard < 16; ++guard) {
        const Stage before = job.stage;
        std::int64_t wait = 0;
        switch (job.stage) {
        case Stage::Received:
            wait = step_received(job);
            break;
        case Stage::Downloading:
            wait = step_downloading(job);
            break;
        case Stage::Downloaded:
            wait = step_downloaded(job);
            break;
        case Stage::Verified:
            wait = step_verified(job);
            break;
        case Stage::Installing:
            wait = step_installing(job);
            break;
        case Stage::Installed:
            wait = step_installed(job);
            break;
        case Stage::Rebooting:
            wait = step_rebooting(job);
            break;
        case Stage::Confirming:
            wait = step_confirming(job);
            break;
        case Stage::Done:
            return kIdleMs;
        }
        if (job.stage == before || wait > 0 || !keep_going()) {
            return wait > 0 ? wait : kIdleMs;
        }
    }
    return kIdleMs;
}

std::int64_t Engine::step_received(Job& job)
{
    ArtifactHandler* handler = handler_for(job);
    if (handler == nullptr) {
        std::string known;
        for (const auto& h : handlers_) {
            known += (known.empty() ? "" : ", ") + h->format();
        }
        fail(job, FailStage::Install, "FORMAT_UNSUPPORTED",
             "formato '" + job.manifest.artifact_format +
                 "' não é instalado por este agente (só " + known + ")",
             false);
        return 0;
    }
    if (auto refused = handler->accept(job.manifest)) {
        fail(job, *refused);
        return 0;
    }
    // Sem assinatura não adianta baixar o que não se assina sozinho.
    if (handler->signature() == ArtifactHandler::Signature::Required &&
        (job.manifest.signature.empty() || job.manifest.signing_key_id.empty())) {
        fail(job, FailStage::Verify, "SIGNATURE_REQUIRED",
             "o formato '" + job.manifest.artifact_format +
                 "' exige signature/signingKeyId no manifesto",
             false);
        return 0;
    }
    const std::int64_t now = platform_.wall_ms();
    if (now < job.manifest.apply_after_ms) {
        return std::min<std::int64_t>(job.manifest.apply_after_ms - now, 60000);
    }
    job.stage = Stage::Downloading;
    job.attempt = 0;
    job.next_try_ms = 0;
    save(job);
    return 0;
}

std::int64_t Engine::step_downloading(Job& job)
{
    const std::int64_t now = platform_.wall_ms();
    if (now < job.next_try_ms) {
        return std::min<std::int64_t>(job.next_try_ms - now, 60000);
    }
    const std::string& id = job.manifest.campaign_item_id;
    const std::string token = platform_.device_token();
    if (token.empty()) {
        fail(job, FailStage::Download, "NO_DEVICE_TOKEN", "sem token do device (device-auth)",
             true);
        return 0;
    }

    const std::string part = part_path(job);
    const std::int64_t have = std::max<std::int64_t>(fs::file_size(part), 0);
    if (job.manifest.size_bytes > 0) {
        const std::int64_t need = job.manifest.size_bytes - have + kSpaceMargin;
        const std::int64_t free = platform_.free_bytes(paths_.download);
        if (free >= 0 && free < need) {
            fail(job, FailStage::Download, "NO_SPACE",
                 "livre " + std::to_string(free) + " bytes em " + paths_.download + ", precisa " +
                     std::to_string(need),
                 true);
            return 0;
        }
    }

    ++job.attempt;
    save(job);
    Json started = {{"attempt", job.attempt}};
    if (job.manifest.size_bytes > 0) {
        started["sizeBytes"] = job.manifest.size_bytes;
    }
    emit(job, "ota-download-started", started);

    DownloadRequest request;
    request.url = job.manifest.artifact_url;
    request.token = token;
    request.path = part;
    request.expected_size = job.manifest.size_bytes;
    const std::int64_t t0 = platform_.mono_ms();
    const DownloadResult r = downloader_.fetch(request, keep_going_);
    job.download_ms += platform_.mono_ms() - t0;
    job.bytes = r.total_bytes;

    switch (r.outcome) {
    case DownloadResult::Outcome::Ok:
        if (std::rename(part.c_str(), bundle_path(job).c_str()) != 0) {
            fail(job, FailStage::Download, "STORAGE", "não renomeou " + part, true);
            return 0;
        }
        job.stage = Stage::Downloaded;
        save(job);
        emit(job, "ota-download-completed", {{"bytes", job.bytes}, {"durationMs", job.download_ms}});
        return 0;
    case DownloadResult::Outcome::Stopped:
        --job.attempt;  // interrompido por nós: não conta
        save(job);
        return kIdleMs;
    case DownloadResult::Outcome::Permanent:
        fail(job, FailStage::Download, r.code.empty() ? "DOWNLOAD_FAILED" : r.code, r.message,
             r.retryable);
        return 0;
    case DownloadResult::Outcome::Transient:
        break;
    }
    if (job.attempt >= kMaxDownloadAttempts) {
        fail(job, FailStage::Download, r.code.empty() ? "DOWNLOAD_FAILED" : r.code,
             r.message + " (" + std::to_string(job.attempt) + " tentativas)", true);
        return 0;
    }
    job.next_try_ms = platform_.wall_ms() + backoff_ms(job.attempt);
    save(job);
    DEXTRO_IOT_WARN("ota %s: download falhou (%s: %s); tentativa %d de %d em %lld s", id.c_str(),
                    r.code.c_str(), r.message.c_str(), job.attempt + 1, kMaxDownloadAttempts,
                    static_cast<long long>(backoff_ms(job.attempt) / 1000));
    return backoff_ms(job.attempt);
}

/// A assinatura ed25519 do manifesto sobre o sha256 que acabou de conferir.
/// `false` = falhou (o `ota-failed` já saiu). Nada aqui é retentável: a
/// assinatura e a chave são da publicação, e baixar de novo não muda nenhuma.
bool Engine::verify_signature(Job& job, const std::string& sha)
{
    std::string why;
    switch (signature::verify_sha256(paths_.keyring, job.manifest.signing_key_id, sha,
                                     job.manifest.signature, why)) {
    case signature::Verdict::Valid:
        return true;
    case signature::Verdict::KeyUnknown:
    case signature::Verdict::KeyInvalid:
        fail(job, FailStage::Verify, "UNKNOWN_SIGNING_KEY", why, false);
        return false;
    case signature::Verdict::Malformed:
    case signature::Verdict::Invalid:
        break;
    }
    fail(job, FailStage::Verify, "SIGNATURE_INVALID", why, false);
    return false;
}

std::int64_t Engine::step_downloaded(Job& job)
{
    const std::string bundle = bundle_path(job);
    const std::int64_t size = fs::file_size(bundle);
    if (size < 0) {
        // O arquivo sumiu (limpeza manual, partição refeita): baixa de novo.
        job.stage = Stage::Downloading;
        job.next_try_ms = 0;
        save(job);
        return 0;
    }
    if (job.manifest.size_bytes > 0 && size != job.manifest.size_bytes) {
        fs::remove_file(bundle);
        fail(job, FailStage::Verify, "SIZE_MISMATCH",
             "baixou " + std::to_string(size) + " bytes, o manifesto diz " +
                 std::to_string(job.manifest.size_bytes),
             true);
        return 0;
    }
    const auto sha = sha256_file(bundle, keep_going_);
    if (!sha) {
        if (!keep_going()) {
            return kIdleMs;
        }
        fail(job, FailStage::Verify, "READ_FAILED", "não leu " + bundle, true);
        return 0;
    }
    if (*sha != job.manifest.sha256) {
        fs::remove_file(bundle);
        fail(job, FailStage::Verify, "CHECKSUM_MISMATCH",
             "sha256 " + *sha + " não é o do manifesto " + job.manifest.sha256, true);
        return 0;
    }
    ArtifactHandler* handler = handler_for(job);
    if (handler == nullptr) {
        fs::remove_file(bundle);
        fail(job, FailStage::Install, "FORMAT_UNSUPPORTED",
             "sem handler para '" + job.manifest.artifact_format + "'", false);
        return 0;
    }
    const auto policy = handler->signature();
    if (policy == ArtifactHandler::Signature::Required ||
        (policy == ArtifactHandler::Signature::IfPresent && !job.manifest.signature.empty())) {
        if (!verify_signature(job, *sha)) {
            fs::remove_file(bundle);
            return 0;
        }
    }
    if (auto refused = handler->check(job.manifest, bundle)) {
        fs::remove_file(bundle);
        Failure f = *refused;
        fail(job, f);
        return 0;
    }
    job.stage = Stage::Verified;
    save(job);
    emit(job, "ota-verified", {{"sha256", *sha}});
    return 0;
}

std::int64_t Engine::step_verified(Job& job)
{
    // O `ota-install-started` é do handler: ele sabe o slot (o do RAUC sai
    // antes do `rauc install`; um formato entregue a outro daemon sai quando
    // o daemon aceita).
    job.stage = Stage::Installing;
    job.handler = Json::object();
    save(job);
    return 0;
}

std::int64_t Engine::step_installing(Job& job)
{
    ArtifactHandler* handler = handler_for(job);
    if (handler == nullptr) {
        fail(job, FailStage::Install, "FORMAT_UNSUPPORTED",
             "sem handler para '" + job.manifest.artifact_format + "'", false);
        return 0;
    }
    const std::string bundle = bundle_path(job);
    if (!fs::exists(bundle)) {
        fail(job, FailStage::Install, "BUNDLE_MISSING", "o artefato sumiu antes da instalação",
             true);
        return 0;
    }

    InstallContext ctx{job, bundle, platform_, keep_going_, [this, &job] { return save(job); },
                       [this, &job](std::string_view code, Json args) {
                           emit(job, code, std::move(args));
                       }};
    InstallStep step = handler->install(ctx);

    switch (step.kind) {
    case InstallStep::Kind::Pending:
        save(job);
        return step.wait_ms > 0 ? step.wait_ms : kIdleMs;
    case InstallStep::Kind::Failed:
        fail(job, step.failure);
        return 0;
    case InstallStep::Kind::Completed:
        emit(job, "ota-installed", step.extra);
        emit(job, "ota-completed", step.extra);
        finish(job, "completed");
        return 0;
    case InstallStep::Kind::Reboot:
        break;
    }
    // Já foi para o slot: libera a partição de dados.
    fs::remove_file(bundle);
    if (job.target_slot.empty()) {
        fail(job, FailStage::Install, "SLOT_UNKNOWN",
             "o handler '" + handler->format() + "' pediu reboot sem slot alvo", false);
        return 0;
    }
    job.stage = Stage::Installed;
    save(job);
    Json installed = step.extra.is_object() ? step.extra : Json::object();
    installed["slot"] = job.target_slot;
    emit(job, "ota-installed", installed);
    return 0;
}

std::int64_t Engine::step_installed(Job& job)
{
    job.install_boot_id = platform_.boot_id();
    job.stage = Stage::Rebooting;
    job.reboot_tries = 0;
    job.next_try_ms = 0;
    save(job);
    emit(job, "ota-rebooting");
    return 0;
}

std::int64_t Engine::step_rebooting(Job& job)
{
    if (platform_.boot_id() != job.install_boot_id) {
        job.stage = Stage::Confirming;
        save(job);
        confirm_since_ms_ = platform_.mono_ms();
        mark_good_failed_since_ms_ = -1;
        DEXTRO_IOT_INFO("ota %s: boot novo depois da instalação (alvo %s, corrente %s)",
                        job.manifest.campaign_item_id.c_str(), job.target_slot.c_str(),
                        platform_.current_slot().c_str());
        return 0;
    }
    const std::int64_t now = platform_.wall_ms();
    if (now < job.next_try_ms) {
        return std::min<std::int64_t>(job.next_try_ms - now, 60000);
    }
    ++job.reboot_tries;
    job.next_try_ms = now + kRebootRetryMs;
    save(job);
    if (platform_.reboot()) {
        DEXTRO_IOT_INFO("ota %s: reboot pedido para subir o slot %s",
                        job.manifest.campaign_item_id.c_str(), job.target_slot.c_str());
        return kRebootRetryMs;
    }
    DEXTRO_IOT_ERR("ota %s: o pedido de reboot falhou (%d)", job.manifest.campaign_item_id.c_str(),
                   job.reboot_tries);
    // A imagem já está no slot e o bootloader já aponta para ele: o item não
    // termina aqui. Conta ao backend uma vez, e continua tentando — o primeiro
    // reboot que acontecer (inclusive manual) é confirmado normalmente.
    if (job.reboot_tries >= kMaxRebootTries && !job.reboot_failure_reported) {
        job.reboot_failure_reported = true;
        save(job);
        emit(job, "ota-failed",
             {{"stage", std::string(fail_stage_name(FailStage::Reboot))},
              {"code", "REBOOT_FAILED"},
              {"message", "o reboot falhou " + std::to_string(job.reboot_tries) +
                              " vezes; a imagem nova sobe no próximo boot"},
              {"retryable", false}});
    }
    return kRebootRetryMs;
}

std::int64_t Engine::step_confirming(Job& job)
{
    const std::int64_t now = platform_.mono_ms();
    if (confirm_since_ms_ < 0) {
        confirm_since_ms_ = now;
    }
    const std::string current = platform_.current_slot();
    // Sem caixa: o `bootname` do RAUC e o valor da cmdline podem vir em caixas
    // diferentes (`ssat.slot=a` e o bootname `A`).
    if (!same_slot(current, job.target_slot)) {
        // Voltou ao slot antigo: o bootloader esgotou as tentativas da janela
        // de teste e marcou o alvo como `bad`, ou o boot caiu no slot anterior
        // por outro caminho.
        const bool gave_up = platform_.slot_bad(job.target_slot);
        emit(job, "ota-rolled-back",
             {{"fromSlot", job.target_slot},
              {"toSlot", current},
              {"reason", gave_up ? "bootloader-giveup" : "booted-previous-slot"}});
        finish(job, "rolled-back");
        return 0;
    }
    switch (platform_.mark_good()) {
    case MarkGood::Good:
        emit(job, "ota-completed", {{"slot", current}});
        finish(job, "completed");
        return 0;
    case MarkGood::Failed:
        // Com a janela de teste aberta, quem falhou o mark-good reinicia a
        // placa (e o bootloader conta a tentativa); o item continua no
        // próximo boot. Se a placa não caiu no prazo, não vai cair.
        if (mark_good_failed_since_ms_ < 0) {
            mark_good_failed_since_ms_ = now;
        }
        if (now - mark_good_failed_since_ms_ >= kMarkGoodFailedGraceMs) {
            fail(job, FailStage::MarkGood, "MARK_GOOD_FAILED",
                 "o mark-good falhou no slot " + current + " e a placa não reiniciou", false);
            return 0;
        }
        return kIdleMs;
    case MarkGood::Pending:
        break;
    }
    if (now - confirm_since_ms_ >= kConfirmTimeoutMs) {
        fail(job, FailStage::MarkGood, "MARK_GOOD_TIMEOUT",
             "o slot " + current + " não foi marcado bom em " +
                 std::to_string(kConfirmTimeoutMs / 60000) + " min",
             false);
        return 0;
    }
    return kIdleMs;
}

}  // namespace dextro::iot::ota
