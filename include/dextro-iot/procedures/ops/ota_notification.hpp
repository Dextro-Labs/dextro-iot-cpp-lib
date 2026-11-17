#pragma once

#include <dextro-iot/procedures/procedure_table.hpp>

#include <string>
#include <vector>

/// `ota-notification` (SPEC do dextrolabs-device §4, contracts.md A.4.3).
///
/// **Esta op não baixa nem verifica o artefato.** Confere o que dá para
/// conferir sem o arquivo — a forma do manifesto, a forma da assinatura, a
/// chave, se a versão já está instalada — e entrega o manifesto pelo
/// `OtaSpool`. A resposta é 202; a trilha (`ota-received` e o resto) segue pela
/// fila de eventos (`events-write`). Quem baixa e instala é o `ota::Engine`
/// (módulo `ota`) ou o agente da aplicação.
///
///   - `version`: `[A-Za-z0-9._+-]{1,64}` — o carimbo da imagem
///     (`20260927202307`) ou semver;
///   - `artifactFormat`: um de `Options::formats` (padrão: o enum do contrato,
///     `swu|raucb|bin|tar.zst|apk|dxu`, contracts.md A.4.3);
///   - `signature` (ed25519, base64 de 64 bytes): obrigatória, menos nos
///     formatos de `Options::self_signed` (padrão `raucb` e `dxu`, o `if` do
///     schema do contrato: o bundle traz a assinatura dele). Presente, a forma
///     é conferida em qualquer formato;
///   - `campaignItemId`: uuid.
///
/// Respostas: 202 entregue; 200 `{alreadyInstalled: true}` quando
/// `InstalledVersion` diz que essa versão já está lá; 400 `CHECKSUM_INVALID`,
/// `SIGNATURE_INVALID`, `UNKNOWN_SIGNING_KEY` ou `INVALID_ARGS`; 500
/// `STORAGE` quando o spool não gravou; 500 `UNAVAILABLE` sem spool.
///
/// Downgrade não é recusado aqui: se uma versão anterior é permitida é
/// política da campanha e do agente.
///
/// Origem: `src/drivers/iot/procedures/ops/ota_notification.cpp` do
/// ssat-traffic-controller (`origin/devel` 67a0f77). Lá as regras de "qual
/// formato não compara com a versão da imagem" (`dxu`, `bin ssatctffwrm`)
/// estavam fixas na op; aqui são da `InstalledVersion` da aplicação.
namespace dextro::iot::procedures {

class OtaNotification final : public Procedure {
public:
    class Options {
    public:
        /// Os `artifactFormat` aceitos. Ausente no payload = aceito (o
        /// campo é opcional no contrato).
        std::vector<std::string> formats = {"swu", "raucb", "bin", "tar.zst", "apk", "dxu"};
        /// Os formatos que dispensam `signature`.
        std::vector<std::string> self_signed = {"raucb", "dxu"};
    };

    /// `keyring` e `installed` podem ser nulos: sem chaveiro, toda chave é
    /// aceita aqui (a recusa fica com o agente, que tem o chaveiro); sem
    /// versão instalada, nunca há `alreadyInstalled`.
    OtaNotification(OtaSpool* spool, const Keyring* keyring, const InstalledVersion* installed);
    OtaNotification(OtaSpool* spool, const Keyring* keyring, const InstalledVersion* installed,
                    Options options);

    std::optional<Reply> validate(const Json& payload) const override;
    Reply execute(const Request& request) override;

    /// Base64 com padding de exatamente 64 bytes (88 caracteres, `==` no fim).
    static bool is_ed25519_base64(std::string_view text) noexcept;

private:
    OtaSpool* spool_;
    const Keyring* keyring_;
    const InstalledVersion* installed_;
    Options options_;
};

}  // namespace dextro::iot::procedures
