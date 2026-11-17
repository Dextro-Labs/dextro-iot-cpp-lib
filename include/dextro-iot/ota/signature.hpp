#pragma once

#include <string>
#include <string_view>

/// A assinatura ed25519 dos artefatos que não se assinam sozinhos.
///
/// O que a pipeline assina (`Artefato.assinarOta` do dextro-infra): ed25519
/// sobre os **32 bytes crus do sha256** do arquivo — não sobre o arquivo, nem
/// sobre o hex. O backend repassa a assinatura (`signature`, base64) e o
/// `signingKeyId` no `ota-notification`; a chave pública vem da IMAGEM, em
/// `<chaveiro>/<signingKeyId>.pub` (PEM SubjectPublicKeyInfo). Uma chave que
/// não está na imagem não assina nada que o device aplique.
///
/// Origem: `src/ota/signature.cpp` do ssat-ota-agent (`origin/devel` 76aa940).
namespace dextro::iot::ota::signature {

enum class Verdict {
    Valid,
    KeyUnknown,  ///< id fora de `[A-Za-z0-9._-]{1,64}` ou sem `<id>.pub` no chaveiro
    KeyInvalid,  ///< o `.pub` existe mas não é uma chave ed25519 PEM
    Malformed,   ///< a assinatura não é base64 de 64 bytes, ou o sha256 não é hex
    Invalid,     ///< não confere
};

/// Confere `signature_b64` sobre o sha256 `sha256_hex` com a chave
/// `<keyring_dir>/<key_id>.pub`. `why` diz o motivo quando não é `Valid`.
Verdict verify_sha256(const std::string& keyring_dir, std::string_view key_id,
                      std::string_view sha256_hex, std::string_view signature_b64,
                      std::string& why);

}  // namespace dextro::iot::ota::signature
