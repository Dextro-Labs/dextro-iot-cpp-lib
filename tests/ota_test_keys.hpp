#pragma once

/// Um par ed25519 de teste, gerado na hora, e a assinatura do jeito que a
/// pipeline faz (`Artefato.assinarOta`: ed25519 sobre os 32 bytes do sha256).
/// Nenhuma chave de verdade entra no repositório.
///
/// Origem: `tests/unit/test_keys.hpp` do ssat-ota-agent (sem a imagem do
/// MCUboot, que é do SSAT).

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <cstdio>
#include <memory>
#include <string>

namespace dextro::iot::ota::test {

class TestKey {
public:
    TestKey() : key_(EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"), &EVP_PKEY_free) {}

    bool valid() const { return key_ != nullptr; }

    /// Grava a pública em PEM (SubjectPublicKeyInfo) em `path`.
    bool write_public(const std::string& path) const
    {
        std::FILE* f = std::fopen(path.c_str(), "w");
        if (f == nullptr) {
            return false;
        }
        const bool ok = PEM_write_PUBKEY(f, key_.get()) == 1;
        std::fclose(f);
        return ok;
    }

    /// Assina os 32 bytes do sha256 `sha_hex`; devolve base64 com padding.
    std::string sign_sha256(const std::string& sha_hex) const
    {
        unsigned char digest[32];
        for (std::size_t i = 0; i < 32; ++i) {
            digest[i] = static_cast<unsigned char>(std::stoi(sha_hex.substr(2 * i, 2), nullptr, 16));
        }
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),
                                                                     &EVP_MD_CTX_free);
        unsigned char sig[64];
        std::size_t len = sizeof sig;
        if (EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key_.get()) != 1 ||
            EVP_DigestSign(ctx.get(), sig, &len, digest, sizeof digest) != 1 || len != 64) {
            return {};
        }
        unsigned char out[89] = {};
        EVP_EncodeBlock(out, sig, 64);
        return reinterpret_cast<const char*>(out);
    }

private:
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key_;
};

}  // namespace dextro::iot::ota::test
