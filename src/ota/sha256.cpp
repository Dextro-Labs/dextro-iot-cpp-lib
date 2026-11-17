#include <dextro-iot/ota/sha256.hpp>

#include <openssl/evp.h>

#include <cstdio>
#include <memory>

namespace dextro::iot::ota {

std::optional<std::string> sha256_file(const std::string& path,
                                       const std::function<bool()>& keep_going)
{
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> f(std::fopen(path.c_str(), "rbe"),
                                                      &std::fclose);
    if (!f) {
        return std::nullopt;
    }
    std::unique_ptr<EVP_MD_CTX, void (*)(EVP_MD_CTX*)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
        return std::nullopt;
    }
    static constexpr std::size_t kBlock = 256 * 1024;
    std::unique_ptr<unsigned char[]> buf(new unsigned char[kBlock]);
    std::size_t n = 0;
    while ((n = std::fread(buf.get(), 1, kBlock, f.get())) > 0) {
        if (EVP_DigestUpdate(ctx.get(), buf.get(), n) != 1) {
            return std::nullopt;
        }
        if (keep_going && !keep_going()) {
            return std::nullopt;
        }
    }
    if (std::ferror(f.get()) != 0) {
        return std::nullopt;
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest, &len) != 1) {
        return std::nullopt;
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2U);
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(kHex[digest[i] >> 4U]);
        out.push_back(kHex[digest[i] & 0x0FU]);
    }
    return out;
}

}  // namespace dextro::iot::ota
