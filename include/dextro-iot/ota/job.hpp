#pragma once

#include <dextro-iot/ota/manifest.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

/// Um item de campanha em andamento, persistido em
/// `<state>/<campaignItemId>.json` a cada passo.
///
/// É o que faz a atualização atravessar o reboot que ela mesma pede: a etapa
/// `rebooting` guarda o `boot_id` em que a instalação rodou, e o boot
/// seguinte — outro `boot_id`, talvez outro slot, talvez outra versão do
/// agente — sabe de onde continuar. Por isso a pasta `state` tem de ficar
/// FORA dos slots A/B (a partição de dados).
///
/// Origem: `src/ota/job.cpp` do ssat-ota-agent (`origin/devel` 76aa940). Os
/// campos `pwr*`/`mcu*` dos formatos do SSAT viraram o `handler`: o estado que
/// cada `ArtifactHandler` precisa guardar entre voltas e reinícios.
namespace dextro::iot::ota {

enum class Stage {
    Received,     ///< esperando o applyAfter
    Downloading,  ///< baixando (com retomada) ou esperando a próxima tentativa
    Downloaded,   ///< arquivo inteiro, falta conferir
    Verified,     ///< tamanho, sha256 (e assinatura) conferidos
    Installing,   ///< o `ArtifactHandler::install` em curso
    Installed,    ///< o slot alvo recebeu a imagem (só formatos com reboot)
    Rebooting,    ///< reboot pedido; esperando um boot_id novo
    Confirming,   ///< boot novo: esperando o mark-good ou constatando o rollback
    Done,         ///< terminou (qualquer resultado); falta só limpar
};

std::string_view stage_name(Stage stage) noexcept;
std::optional<Stage> stage_from_name(std::string_view name) noexcept;

class Job {
public:
    Manifest manifest;
    Stage stage = Stage::Received;
    int attempt = 0;               ///< tentativas de download feitas
    std::int64_t next_try_ms = 0;  ///< relógio de parede; 0 = já
    std::int64_t download_ms = 0;  ///< tempo somado das tentativas
    std::int64_t bytes = 0;
    std::string from_slot;
    std::string target_slot;
    std::string install_boot_id;
    int reboot_tries = 0;
    bool reboot_failure_reported = false;
    std::string result;  ///< em Done: completed | rolled-back | failed
    /// O estado do `ArtifactHandler` deste formato (objeto JSON, gravado junto).
    nlohmann::json handler = nlohmann::json::object();

    nlohmann::json to_json() const;
    static std::optional<Job> from_json(const nlohmann::json& j);
};

}  // namespace dextro::iot::ota
