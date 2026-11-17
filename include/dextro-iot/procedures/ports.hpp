#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// O que as device-procedures genéricas precisam da aplicação, como interfaces.
///
/// Cada op recebe só os ports de que precisa, na construção. A aplicação
/// implementa (status, settings, sessão) ou usa a implementação da biblioteca
/// (`certificates/certificate_store.hpp`, OpenSSL); os testes passam fakes. É
/// essa fronteira que deixa as ops sem arquivo, sem relógio e sem MQTT.
///
/// **Chamados da thread que roda o `Client::tick()`.** Toda implementação tem
/// que ser segura para isso.
///
/// Origem: `src/drivers/iot/procedures/ports.hpp` do ssat-traffic-controller.
/// Os ports do controlador de trânsito (anéis, programação, semente de teste)
/// ficaram lá: são da aplicação. O `OtaSpool` de lá virou três portas aqui
/// (`OtaSpool`, `Keyring`, `InstalledVersion`): no SSAT as três respostas
/// vinham de um objeto só, e em outro device vêm de lugares diferentes.
namespace dextro::iot::procedures {

using Json = nlohmann::json;

/// `get-status`: um bloco por seção.
class StatusSource {
public:
    virtual ~StatusSource() = default;

    /// As seções que esta fonte conhece, na ordem em que saem em `"all"`.
    virtual std::vector<std::string> sections() const = 0;

    /// Preenche `out` com a seção. `false` = seção desconhecida.
    virtual bool fill(std::string_view section, Json& out) const = 0;
};

/// Um item rejeitado de `settings-write`.
class Rejection {
public:
    std::string ref;
    std::string reason;
};

/// `settings-read` / `settings-write` sobre a coleção de settings do device
/// (contracts.md A.1.1 `SettingsCollection`; SPEC do dextrolabs-device §8).
class SettingsStore {
public:
    virtual ~SettingsStore() = default;

    /// A versão aplicada, 0 quando nunca houve `settings-write`.
    virtual std::int64_t version() const = 0;

    /// A coleção aplicada, filtrada por `refs` (vazio = todas). Um ref que não
    /// casa com nada é ignorado — `settings-read` não tem como recusar item.
    virtual Json read(const std::vector<std::string>& refs) const = 0;

    /// Aplica a coleção e grava `version`. Item que não pode ser aplicado vai
    /// para `rejected`; o resto é aplicado. Devolve `false` quando não deu nem
    /// para persistir — e aí a versão NÃO avança. `session_affected` = algum
    /// item mudou o broker ou a credencial: a sessão será reaberta.
    virtual bool apply(std::int64_t version, const Json& settings, std::vector<Rejection>& rejected,
                       bool& session_affected) = 0;
};

/// `certificate-request` / `certificate-install` (SPEC do dextrolabs-device §3.5).
class CertificateStore {
public:
    enum class Result {
        Ok,
        KeyGeneration,  ///< não deu para gerar a chave ou a CSR
        Mismatch,       ///< o certificado não casa com nenhuma chave pendente
        Invalid,        ///< PEM ilegível
        Storage,        ///< não deu para gravar
    };

    virtual ~CertificateStore() = default;

    /// Gera um par novo e devolve a CSR em PEM. O mesmo `request_id` repetido
    /// devolve a MESMA CSR — é reentrega, não pedido novo (A.4.7).
    virtual Result request_csr(std::string_view request_id, std::string& csr_pem) = 0;

    /// Instala o certificado emitido para uma chave pendente e devolve o
    /// SHA-256 do DER, em hex.
    virtual Result install(std::string_view cert_pem, std::string_view chain_pem,
                           std::string& fingerprint) = 0;
};

/// `ota-notification` (contracts.md A.4.3): para onde vai o manifesto aceito.
///
/// A op não baixa nem instala: confere o que dá para conferir sem o arquivo e
/// entrega. Quem baixa é o `ota::Engine` (módulo `ota`, outra thread ou outro
/// processo), ou o agente que a aplicação tiver. `InboxOtaSpool`
/// (`procedures/ota_adapters.hpp`) é a entrega por arquivo no inbox do
/// `Engine`.
class OtaSpool {
public:
    virtual ~OtaSpool() = default;

    /// Grava o manifesto (o payload da op, como veio) para o agente. A mesma
    /// `campaignItemId` de novo sobrescreve: reentrega é idempotente. `false`
    /// = não gravou (a op responde 500 `STORAGE` e o backend tenta de novo).
    virtual bool enqueue(const Json& manifest) = 0;
};

/// As chaves ed25519 em que o device confia para os artefatos que não se
/// assinam sozinhos (`signingKeyId` do `ota-notification`).
class Keyring {
public:
    virtual ~Keyring() = default;

    /// `true` quando `key_id` é uma chave conhecida. A op só pergunta com id
    /// não vazio.
    virtual bool known(std::string_view key_id) const = 0;
};

/// A versão instalada do que o manifesto atualiza, para o `alreadyInstalled`
/// (200 sem entregar).
class InstalledVersion {
public:
    virtual ~InstalledVersion() = default;

    /// A versão hoje instalada do artefato que `manifest` descreve
    /// (`artifactFormat`, `artifactName`), ou `nullopt` quando este processo
    /// não sabe — e então a comparação fica com o agente. Ex.: a versão da
    /// IMAGEM para o `raucb`; `nullopt` para o firmware de uma placa que só o
    /// agente consulta.
    virtual std::optional<std::string> installed_version(const Json& manifest) const = 0;
};

/// O que uma op pode pedir à sessão com o broker. Assíncrono de propósito:
/// quem executa é a aplicação, DEPOIS de a resposta ser publicada — um
/// `certificate-install` que reconectasse antes de responder deixaria o
/// backend esperando um reply que nunca sai.
class SessionControl {
public:
    virtual ~SessionControl() = default;

    /// `outbox-notification`: drenar a outbox na próxima volta.
    virtual void request_outbox_drain() = 0;

    /// Reabrir a sessão com a configuração e o certificado atuais.
    virtual void request_reconnect() = 0;

    /// `true` enquanto a sessão está autenticada pelo certificado de fábrica.
    /// Nesse escopo só as ops marcadas `bootstrap` são atendidas (A.4).
    virtual bool bootstrap_scope() const = 0;
};

}  // namespace dextro::iot::procedures
