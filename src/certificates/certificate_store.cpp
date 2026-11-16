#include <dextro-iot/certificates/certificate_store.hpp>

#include <dextro-iot/atomic_file.hpp>
#include <dextro-iot/log.hpp>

// O ÚNICO lugar do projeto que inclui o OpenSSL. Ver o header.
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <dirent.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace dextro::iot {
namespace {

using Result = procedures::CertificateStore::Result;

// Donos RAII para os tipos do OpenSSL. Cada caminho de erro abaixo sai com
// `return`, e um `*_free` esquecido num deles é vazamento por renovação —
// pouco por vez, mas a placa fica anos ligada.
struct BioFree {
    void operator()(BIO* p) const noexcept { BIO_free(p); }
};
struct KeyFree {
    void operator()(EVP_PKEY* p) const noexcept { EVP_PKEY_free(p); }
};
struct ReqFree {
    void operator()(X509_REQ* p) const noexcept { X509_REQ_free(p); }
};
struct CertFree {
    void operator()(X509* p) const noexcept { X509_free(p); }
};
struct ExtsFree {
    void operator()(STACK_OF(X509_EXTENSION)* p) const noexcept
    {
        sk_X509_EXTENSION_pop_free(p, X509_EXTENSION_free);
    }
};

using Bio = std::unique_ptr<BIO, BioFree>;
using Key = std::unique_ptr<EVP_PKEY, KeyFree>;
using Req = std::unique_ptr<X509_REQ, ReqFree>;
using Cert = std::unique_ptr<X509, CertFree>;
using Exts = std::unique_ptr<STACK_OF(X509_EXTENSION), ExtsFree>;

/// O `requestId` vira nome de arquivo. O `certificate-request` já exige uuid,
/// mas este store não confia em quem o chama para isso: um `../` aqui seria
/// escrita arbitrária em /data com os privilégios do daemon.
bool safe_name(std::string_view id) noexcept
{
    if (id.empty() || id.size() > 64) {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                        (c >= 'A' && c <= 'F') || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::string bio_text(BIO* bio)
{
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    return size > 0 ? std::string(data, static_cast<std::size_t>(size)) : std::string{};
}

bool add_extension(STACK_OF(X509_EXTENSION)* exts, int nid, const char* value)
{
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, nullptr, nid, value);
    if (ext == nullptr) {
        return false;
    }
    if (sk_X509_EXTENSION_push(exts, ext) == 0) {
        X509_EXTENSION_free(ext);
        return false;
    }
    return true;
}

/// Gera o par e a CSR. Tudo em memória: nada vai a disco antes de dar certo.
bool generate(const std::string& common_name, std::string& key_pem, std::string& csr_pem)
{
    Key key(EVP_EC_gen("P-256"));
    if (!key) {
        return false;
    }

    Req req(X509_REQ_new());
    if (!req || X509_REQ_set_version(req.get(), X509_REQ_VERSION_1) != 1 ||
        X509_REQ_set_pubkey(req.get(), key.get()) != 1) {
        return false;
    }

    X509_NAME* name = X509_REQ_get_subject_name(req.get());
    if (X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_UTF8,
            reinterpret_cast<const unsigned char*>(common_name.c_str()), -1, -1, 0) != 1) {
        return false;
    }

    // Só as duas que o service aceita. Nenhum SAN.
    Exts exts(sk_X509_EXTENSION_new_null());
    if (!exts || !add_extension(exts.get(), NID_key_usage, "critical,digitalSignature") ||
        !add_extension(exts.get(), NID_ext_key_usage, "clientAuth") ||
        X509_REQ_add_extensions(req.get(), exts.get()) != 1) {
        return false;
    }

    if (X509_REQ_sign(req.get(), key.get(), EVP_sha256()) <= 0) {
        return false;
    }

    Bio key_bio(BIO_new(BIO_s_mem()));
    Bio csr_bio(BIO_new(BIO_s_mem()));
    if (!key_bio || !csr_bio ||
        PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr,
                                 nullptr) != 1 ||
        PEM_write_bio_X509_REQ(csr_bio.get(), req.get()) != 1) {
        return false;
    }
    key_pem = bio_text(key_bio.get());
    csr_pem = bio_text(csr_bio.get());
    return !key_pem.empty() && !csr_pem.empty();
}

Key load_key(const std::string& pem)
{
    Bio bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!bio) {
        return nullptr;
    }
    return Key(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
}

Cert load_cert(std::string_view pem)
{
    Bio bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!bio) {
        return nullptr;
    }
    return Cert(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
}

std::string sha256_hex(X509* cert)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    if (X509_digest(cert, EVP_sha256(), digest, &size) != 1) {
        return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (unsigned int i = 0; i < size; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0f]);
    }
    return out;
}

/// Os `requestId` com chave pendente.
std::vector<std::string> pending_ids(const std::string& dir)
{
    std::vector<std::string> ids;
    DIR* handle = ::opendir(dir.c_str());
    if (handle == nullptr) {
        return ids;
    }
    while (const dirent* entry = ::readdir(handle)) {
        const std::string_view name(entry->d_name);
        constexpr std::string_view kSuffix = ".key";
        if (name.size() > kSuffix.size() &&
            name.substr(name.size() - kSuffix.size()) == kSuffix) {
            ids.emplace_back(name.substr(0, name.size() - kSuffix.size()));
        }
    }
    ::closedir(handle);
    return ids;
}

}  // namespace

FileCertificateStore::FileCertificateStore(std::string common_name, CertificatePaths paths)
    : common_name_(std::move(common_name)), paths_(std::move(paths))
{
}

Result FileCertificateStore::request_csr(std::string_view request_id, std::string& csr_pem)
{
    if (!safe_name(request_id) || common_name_.empty()) {
        return Result::KeyGeneration;
    }
    const std::string base = paths_.pending_dir + "/" + std::string(request_id);

    // Reentrega do mesmo pedido: a MESMA CSR (A.4.7). Gerar outra aqui faria
    // o backend assinar uma chave que o device já jogou fora.
    if (read_file(base + ".csr", csr_pem) && !csr_pem.empty()) {
        std::string ignored;
        if (read_file(base + ".key", ignored)) {
            return Result::Ok;
        }
    }

    std::string key_pem;
    if (!generate(common_name_, key_pem, csr_pem)) {
        DEXTRO_IOT_ERR("dextro-iot: não gerou chave/CSR EC P-256");
        return Result::KeyGeneration;
    }

    (void)::mkdir(paths_.pending_dir.c_str(), 0700);
    // A chave antes da CSR: com a CSR gravada e a chave não, a reentrega
    // devolveria uma CSR sem chave para casar com o certificado.
    if (!write_file_atomic(base + ".key", key_pem, 0600) ||
        !write_file_atomic(base + ".csr", csr_pem, 0644)) {
        DEXTRO_IOT_ERR("dextro-iot: não gravou a chave pendente em %s", paths_.pending_dir.c_str());
        return Result::Storage;
    }
    DEXTRO_IOT_INFO("dextro-iot: CSR gerada para o pedido %.*s", static_cast<int>(request_id.size()),
              request_id.data());
    return Result::Ok;
}

Result FileCertificateStore::install(std::string_view cert_pem, std::string_view chain_pem,
                                     std::string& fingerprint)
{
    Cert cert = load_cert(cert_pem);
    if (!cert) {
        return Result::Invalid;
    }

    // Procura a chave pendente que casa com o certificado. Normalmente há uma
    // só; mais de uma quer dizer pedido reentregue com id novo, e só uma casa.
    std::string match_id;
    std::string key_pem;
    for (const std::string& id : pending_ids(paths_.pending_dir)) {
        std::string candidate;
        if (!read_file(paths_.pending_dir + "/" + id + ".key", candidate)) {
            continue;
        }
        Key key = load_key(candidate);
        if (key && X509_check_private_key(cert.get(), key.get()) == 1) {
            match_id = id;
            key_pem = std::move(candidate);
            break;
        }
    }
    if (match_id.empty()) {
        return Result::Mismatch;
    }

    fingerprint = sha256_hex(cert.get());
    if (fingerprint.empty()) {
        return Result::Invalid;
    }

    // O par atual vira `.prev` ANTES de gravar o novo. Se o broker recusar o
    // certificado novo, é com ele que alguém na bancada volta atrás.
    std::string previous;
    if (read_file(paths_.cert_path, previous)) {
        (void)write_file_atomic(paths_.cert_path + ".prev", previous, 0644);
    }
    if (read_file(paths_.key_path, previous)) {
        (void)write_file_atomic(paths_.key_path + ".prev", previous, 0600);
    }

    // O certificado do cliente vai com a cadeia: o EMQX precisa dos
    // intermediários para chegar à CA que ele conhece.
    std::string bundle(cert_pem);
    if (!bundle.empty() && bundle.back() != '\n') {
        bundle.push_back('\n');
    }
    bundle.append(chain_pem);

    // A chave antes do certificado: com o certificado novo e a chave velha, o
    // TLS falharia no handshake; com a chave nova e o certificado velho, também
    // — mas o `.prev` desfaz os dois do mesmo jeito.
    if (!write_file_atomic(paths_.key_path, key_pem, 0600) ||
        !write_file_atomic(paths_.cert_path, bundle, 0644)) {
        DEXTRO_IOT_ERR("dextro-iot: não gravou o certificado em %s", paths_.cert_path.c_str());
        return Result::Storage;
    }

    // Instalado um, os outros pedidos pendentes ficaram velhos.
    for (const std::string& id : pending_ids(paths_.pending_dir)) {
        const std::string base = paths_.pending_dir + "/" + id;
        (void)::unlink((base + ".key").c_str());
        (void)::unlink((base + ".csr").c_str());
    }

    DEXTRO_IOT_INFO("dextro-iot: certificado instalado (sha256 %.16s…)", fingerprint.c_str());
    return Result::Ok;
}

std::string certificate_fingerprint(const std::string& pem_path)
{
    std::string pem;
    if (!read_file(pem_path, pem) || pem.empty()) {
        return {};
    }
    const Cert cert = load_cert(pem);
    return cert ? sha256_hex(cert.get()) : std::string{};
}

}  // namespace dextro::iot
