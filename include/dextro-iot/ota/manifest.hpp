#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

/// O manifesto do `ota-notification` (contracts.md A.4.3), como o
/// `InboxOtaSpool` o deixa em `<inbox>/<campaignItemId>.json`.
///
/// A op já validou a forma antes de responder 202; aqui se valida de novo,
/// porque o `Engine` não confia num arquivo só por estar numa pasta (pode
/// ser outro processo que o escreveu).
///
/// Origem: `src/ota/manifest.cpp` do ssat-ota-agent (`origin/devel` 76aa940).
namespace dextro::iot::ota {

class Manifest {
public:
    std::string campaign_item_id;
    std::string version;
    std::string artifact_url;
    std::string sha256;  ///< minúsculo
    std::string artifact_format;
    std::string artifact_name;  ///< vazio = o backend não disse
    std::string progress_entity_id;
    /// ed25519 (base64) sobre os 32 bytes do sha256, e o id da chave que
    /// assinou. Vazios = não veio.
    std::string signature;
    std::string signing_key_id;
    std::int64_t size_bytes = 0;      ///< 0 = o backend não disse
    std::int64_t apply_after_ms = 0;  ///< epoch ms; 0 = já

    /// Nada = recusado, e `why` diz por quê. `id_out` recebe o
    /// `campaignItemId` quando ele próprio é válido, para a recusa poder ser
    /// reportada na trilha do item.
    static std::optional<Manifest> parse(std::string_view text, std::string& why,
                                         std::string* id_out = nullptr);

    nlohmann::json to_json() const;
};

/// O id vira nome de arquivo: só `[0-9A-Za-z_-]`, de 1 a 64.
bool is_safe_id(std::string_view id) noexcept;

}  // namespace dextro::iot::ota
