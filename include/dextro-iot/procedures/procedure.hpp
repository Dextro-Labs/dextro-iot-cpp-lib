#pragma once

#include <dextro-iot/procedures/rescode.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

/// O contrato de uma device-procedure: a classe abstrata que cada op implementa.
///
/// ## Por que template method
///
/// `handle()` é `final` e é o único ponto de entrada. Ele valida, executa e
/// **segura qualquer exceção** — a spec §9 manda que payload malformado nunca
/// derrube o processo, e neste processo quem cai leva junto o heartbeat que o
/// coprocessador vigia (no controlador SSAT, de onde isto veio). Com o `try/catch` no pai, nenhuma op consegue esquecer
/// dele: o filho só escreve `validate()` e `execute()`.
///
/// ## O que uma op NÃO faz
///
/// Não abre arquivo, não fala MQTT, não lê relógio. Tudo o que ela precisa do
/// resto do daemon vem por interfaces (as genéricas estão em `ports.hpp`),
/// injetadas na construção. É o que deixa as ops rodarem no CI contra fakes.
///
/// O nome da op (`"get-status"`...) não é da classe: é da linha com que a
/// aplicação a registra na `ProcedureTable`.
namespace dextro::iot::procedures {

using Json = nlohmann::json;

/// O que chegou, já fora do envelope. O envelope (`metadata`, TTL, `v`) é da
/// biblioteca do protocolo; aqui só entra o que a op precisa.
class Request {
public:
    std::string relation_id;
    /// `payload.idempotencyKey`, quando o backend mandou (spec §5). Vazio
    /// quando não.
    std::string idempotency_key;
    Json payload = Json::object();
};

/// O que volta: `rescode` e `payload`, na mesma forma dos dois sentidos
/// (spec §3). Em erro, `payload = {error, detail?}`.
class Reply {
public:
    Rescode rescode = Rescode::Ok;
    Json payload = Json::object();

    static Reply ok(Json payload);
    static Reply accepted(Json payload = Json::object());
    static Reply no_content();
    static Reply error(Rescode rescode, ErrorCode code, std::string detail = {});
    /// O mesmo, com um código de erro da APLICAÇÃO (do contrato do tipo de
    /// device), na grafia do fio: `"INVALID_PLAN"`, `"DOOR_JAMMED"`...
    static Reply error(Rescode rescode, std::string_view code, std::string detail = {});

    /// Um atalho para o caso mais comum: payload fora do schema.
    static Reply invalid(std::string detail);
};

class Procedure {
public:
    virtual ~Procedure() = default;

    Procedure() = default;
    Procedure(const Procedure&) = delete;
    Procedure& operator=(const Procedure&) = delete;

    /// Valida o payload contra o schema da op. `nullopt` = válido; um `Reply`
    /// = a resposta de erro, que volta sem chegar ao `execute()`.
    ///
    /// Separado do `execute()` porque o contrato diz que input inválido é 400
    /// **sem efeito** (SPEC do dextrolabs-device §4.6): uma op que validasse
    /// no meio da execução poderia deixar metade feita.
    virtual std::optional<Reply> validate(const Json& payload) const = 0;

    /// Executa. Só é chamado com payload já validado.
    virtual Reply execute(const Request& request) = 0;

    /// O ponto de entrada. Não lança.
    Reply handle(const Request& request) noexcept;
};

// --- validação --------------------------------------------------------------
// Os pedaços de JSON Schema que as ops repetem. Não é um validador de schema:
// é o subconjunto que o anexo A usa, escrito à mão, e cada função devolve o
// `Reply` de erro já montado com o nome do campo em `detail`.

/// Recusa chave fora de `allowed` (`additionalProperties: false`).
std::optional<Reply> only_keys(const Json& payload, std::initializer_list<std::string_view> allowed);

/// Exige que `key` exista.
std::optional<Reply> require(const Json& payload, std::string_view key);

/// `key` é string com até `max_len` bytes (0 = sem limite).
std::optional<Reply> string_field(const Json& payload, std::string_view key, std::size_t max_len);

/// `key` é inteiro >= `min`.
std::optional<Reply> integer_field(const Json& payload, std::string_view key, std::int64_t min);

bool is_uuid(std::string_view text) noexcept;
bool is_sha256_hex(std::string_view text) noexcept;
bool is_semver(std::string_view text) noexcept;
/// Uma versão como o backend a manda no `ota-notification`: `[A-Za-z0-9._+-]`,
/// de 1 a 64. Semver cabe, e também o carimbo `AAAAMMDDhhmmss` da imagem.
bool is_version_token(std::string_view text) noexcept;

}  // namespace dextro::iot::procedures
