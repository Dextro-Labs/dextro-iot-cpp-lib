#pragma once

#include <dextro-iot/procedures/ports.hpp>

#include <string>

namespace dextro::iot::ota {
class EventSink;
}

/// Implementações por arquivo das portas do `ota-notification`, no formato
/// que o `ota::Engine` lê.
///
/// Origem: `src/services/iot-manager/adapters/spool_ota.cpp` do
/// ssat-traffic-controller (`origin/devel` 67a0f77). Lá o `ota-received` ia
/// para a pasta de arquivos que o próprio trafficd lia; aqui vai para o
/// `EventSink` da aplicação, e a versão instalada saiu para a porta
/// `InstalledVersion` (é da aplicação: a da imagem vem do RAUC, a de uma
/// placa vem da placa).
namespace dextro::iot::procedures {

/// O manifesto em `<inbox>/<campaignItemId>.json`, gravado tmp+fsync+rename.
///
/// ## Por que um diretório
///
/// Quem baixa pode estar em outro processo, ou parado — é o cenário do agente
/// atualizando a si mesmo —, e o manifesto não pode se perder por isso. O
/// arquivo é o *handoff* que sobrevive a qualquer um dos dois estar fora, e
/// serve igual quando o `Engine` é uma thread do mesmo processo. Reentrega do
/// mesmo item sobrescreve o mesmo arquivo: idempotente de graça (o `Engine`
/// ignora o manifesto de um item em andamento).
///
/// ## A trilha
///
/// Gravado o manifesto, sai `ota-received {campaignItemId, version,
/// applyAfter}` no `EventSink` (o schema do evento exige `applyAfter`: 0
/// quando o backend não mandou). Não gravar o evento não desfaz a entrega.
class InboxOtaSpool final : public OtaSpool {
public:
    /// `events` pode ser nulo (sem trilha). Chamado da thread do `Client`.
    InboxOtaSpool(std::string inbox_dir, ota::EventSink* events);

    bool enqueue(const Json& manifest) override;

    const std::string& inbox_dir() const noexcept { return inbox_dir_; }

private:
    std::string inbox_dir_;
    ota::EventSink* events_;
};

/// O chaveiro como diretório: `<dir>/<signingKeyId>.pub` (PEM
/// SubjectPublicKeyInfo ed25519, instalado pela IMAGEM). É o mesmo
/// diretório que o `ota::Engine` usa para conferir a assinatura
/// (`Engine::Paths::keyring`).
///
/// **Enquanto o diretório não existir, aceita qualquer id** e deixa a recusa
/// para o agente: recusar aqui sem chaveiro seria recusar toda campanha numa
/// imagem que ainda não instala chaves.
class DirectoryKeyring final : public Keyring {
public:
    explicit DirectoryKeyring(std::string dir);

    bool known(std::string_view key_id) const override;

private:
    std::string dir_;
};

/// O id vira nome de arquivo: `[0-9A-Za-z_-]{1,64}` (sem `/`, sem `.`).
bool is_safe_file_id(std::string_view id) noexcept;

}  // namespace dextro::iot::procedures
