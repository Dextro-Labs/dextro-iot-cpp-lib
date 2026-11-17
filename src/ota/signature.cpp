#include <dextro-iot/ota/signature.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <cstdio>
#include <memory>

namespace dextro::iot::ota::signature {
namespace {

bool safe_key_id(std::string_view id) noexcept
{
    if (id.empty() || id.size() > 64 || id.front() == '.') {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_' || c == '.';
    });
}

int hex_value(char c) noexcept
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool decode_hex32(std::string_view hex, unsigned char out[32]) noexcept
{
    if (hex.size() != 64) {
        return false;
    }
    for (std::size_t i = 0; i < 32; ++i) {
        const int hi = hex_value(hex[2 * i]);
        const int lo = hex_value(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<unsigned char>((hi << 4) | lo);
    }
    return true;
}

/// Base64 padrão, com padding; exatamente 64 bytes decodificados.
bool decode_signature(std::string_view b64, unsigned char out[64]) noexcept
{
    if (b64.size() != 88 || b64.substr(86) != "==") {
        return false;
    }
    unsigned char buffer[66] = {};
    const int n = EVP_DecodeBlock(buffer, reinterpret_cast<const unsigned char*>(b64.data()),
                                  static_cast<int>(b64.size()));
    // EVP_DecodeBlock não desconta o padding: 88 caracteres viram 66 bytes,
    // dos quais os dois últimos são o "==".
    if (n != 66) {
        return false;
    }
    std::copy(buffer, buffer + 64, out);
    return true;
}

}  // namespace

Verdict verify_sha256(const std::string& keyring_dir, std::string_view key_id,
                      std::string_view sha256_hex, std::string_view signature_b64,
                      std::string& why)
{
    if (!safe_key_id(key_id)) {
        why = "signingKeyId ausente ou fora de [A-Za-z0-9._-]{1,64}";
        return Verdict::KeyUnknown;
    }
    unsigned char digest[32];
    if (!decode_hex32(sha256_hex, digest)) {
        why = "sha256 não é hex de 64";
        return Verdict::Malformed;
    }
    unsigned char sig[64];
    if (!decode_signature(signature_b64, sig)) {
        why = "signature não é ed25519 em base64 (64 bytes)";
        return Verdict::Malformed;
    }

    const std::string path = keyring_dir + "/" + std::string(key_id) + ".pub";
    std::FILE* f = std::fopen(path.c_str(), "re");
    if (f == nullptr) {
        why = "chave " + std::string(key_id) + " não está na imagem (" + path + ")";
        return Verdict::KeyUnknown;
    }
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        PEM_read_PUBKEY(f, nullptr, nullptr, nullptr), &EVP_PKEY_free);
    std::fclose(f);
    if (!key || EVP_PKEY_base_id(key.get()) != EVP_PKEY_ED25519) {
        why = path + " não é uma chave pública ed25519 em PEM";
        return Verdict::KeyInvalid;
    }

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),
                                                                 &EVP_MD_CTX_free);
    // Ed25519 é "one-shot": sem digest próprio (NULL) e um EVP_DigestVerify só.
    if (!ctx || EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
        why = "openssl não preparou a verificação ed25519";
        return Verdict::KeyInvalid;
    }
    if (EVP_DigestVerify(ctx.get(), sig, sizeof sig, digest, sizeof digest) != 1) {
        why = "a assinatura ed25519 não confere com a chave " + std::string(key_id);
        return Verdict::Invalid;
    }
    return Verdict::Valid;
}

}  // namespace dextro::iot::ota::signature
