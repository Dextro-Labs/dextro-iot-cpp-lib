#pragma once

#include <optional>
#include <string_view>

/// O `rescode` da resposta e o `payload.error` que acompanha um erro.
///
/// Os dois como enum: o fio não pode ter
/// string solta. Um `"SIGNATURE_INVALID"` digitado à mão num handler e
/// `"SIGNATURE_INVALD"` noutro é defeito que só o backend vê — e ele marca o
/// item da campanha como `failed` sem retry (contracts.md A.4.3).
namespace dextro::iot::procedures {

/// A tabela única de `rescode` da spec §3 — só os que um device responde.
enum class Rescode : int {
    Ok = 200,
    Accepted = 202,
    NoContent = 204,
    BadRequest = 400,
    Forbidden = 403,
    NotFound = 404,
    Conflict = 409,
    PayloadTooLarge = 413,
    ServerError = 500,
    Timeout = 504,
};

constexpr int value(Rescode code) noexcept
{
    return static_cast<int>(code);
}

constexpr bool success(Rescode code) noexcept
{
    return value(code) >= 200 && value(code) < 300;
}

/// Os códigos de `payload.error` do protocolo e das ops genéricas. Os nomes
/// são os do anexo A dos contratos do dextrolabs-device.
///
/// Os códigos de negócio de cada tipo de device (os do contrato do tipo) NÃO
/// entram aqui: a aplicação usa `Reply::error(Rescode, std::string_view, ...)`
/// com a grafia do fio, e de preferência o seu próprio enum para eles.
enum class ErrorCode {
    InvalidArgs,        ///< payload fora do schema da op
    UnknownOp,          ///< op fora da tabela
    OutOfScope,         ///< op fora do escopo do certificado de bootstrap
    OutboxOnly,         ///< op que só chega pela outbox, pedida pela RPC direta
    Internal,           ///< exceção dentro do handler
    Unavailable,        ///< o recurso de que a op precisa não está ligado
    TtlExpired,         ///< pedido mais velho que o TTL (spec §8)
    ClockSkew,          ///< pedido com timestamp no futuro além da tolerância

    // ota-notification (A.4.3)
    SignatureInvalid,
    ChecksumInvalid,
    VersionNotAllowed,
    UnknownSigningKey,

    // settings-write (A.4.6, e as regras de A.1.1 que o schema não expressa)
    StaleVersion,
    InvalidSetting,

    // certificate-* (A.4.7/A.4.8)
    CsrInvalid,
    KeyGeneration,
    Storage,
};

/// `"INVALID_ARGS"` etc. String estática.
std::string_view error_name(ErrorCode code) noexcept;

/// O inverso, para quem recebe o motivo de outro processo já na grafia do fio. `nullopt` para nome que não está no enum.
std::optional<ErrorCode> error_from_name(std::string_view name) noexcept;

}  // namespace dextro::iot::procedures
