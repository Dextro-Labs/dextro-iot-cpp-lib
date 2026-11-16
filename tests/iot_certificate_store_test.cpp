#include <dextro-iot/atomic_file.hpp>
#include <dextro-iot/certificates/certificate_store.hpp>

#include <gtest/gtest.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#ifndef DEXTRO_IOT_TEST_DIR
#error "DEXTRO_IOT_TEST_DIR não definido — ver tests/CMakeLists.txt"
#endif

using dextro::iot::CertificatePaths;
using dextro::iot::FileCertificateStore;
using Result = dextro::iot::procedures::CertificateStore::Result;

namespace {

const std::string kSerial = "A1B2C3D4E5F6";
const std::string kRequest = "0190f7a2-6c1e-7b3a-9f00-1234567890ab";

/// Uma CA de bancada: é ela que faz o papel da `pki-devices` do OpenBao e
/// assina a CSR que o store gerou.
class TestCa {
public:
    TestCa()
    {
        key_ = EVP_EC_gen("P-256");
        cert_ = X509_new();
        X509_set_version(cert_, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert_), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert_), 0);
        X509_gmtime_adj(X509_getm_notAfter(cert_), 3600);
        X509_NAME* name = X509_get_subject_name(cert_);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
                                   reinterpret_cast<const unsigned char*>("ca de teste"), -1, -1, 0);
        X509_set_issuer_name(cert_, name);
        X509_set_pubkey(cert_, key_);
        X509_sign(cert_, key_, EVP_sha256());
    }
    ~TestCa()
    {
        X509_free(cert_);
        EVP_PKEY_free(key_);
    }
    TestCa(const TestCa&) = delete;
    TestCa& operator=(const TestCa&) = delete;

    /// Assina a CSR em PEM e devolve o certificado em PEM.
    std::string sign(const std::string& csr_pem) const
    {
        BIO* in = BIO_new_mem_buf(csr_pem.data(), static_cast<int>(csr_pem.size()));
        X509_REQ* req = PEM_read_bio_X509_REQ(in, nullptr, nullptr, nullptr);
        BIO_free(in);
        if (req == nullptr) {
            return {};
        }
        EVP_PKEY* pub = X509_REQ_get_pubkey(req);

        X509* cert = X509_new();
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 2);
        X509_gmtime_adj(X509_getm_notBefore(cert), 0);
        X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
        X509_set_subject_name(cert, X509_REQ_get_subject_name(req));
        X509_set_issuer_name(cert, X509_get_subject_name(cert_));
        X509_set_pubkey(cert, pub);
        X509_sign(cert, key_, EVP_sha256());

        std::string out = pem(cert);
        X509_free(cert);
        EVP_PKEY_free(pub);
        X509_REQ_free(req);
        return out;
    }

    std::string chain() const { return pem(cert_); }

private:
    static std::string pem(X509* cert)
    {
        BIO* out = BIO_new(BIO_s_mem());
        PEM_write_bio_X509(out, cert);
        char* data = nullptr;
        const long size = BIO_get_mem_data(out, &data);
        std::string text(data, static_cast<std::size_t>(size));
        BIO_free(out);
        return text;
    }

    EVP_PKEY* key_ = nullptr;
    X509* cert_ = nullptr;
};

class CertificateStore : public ::testing::Test {
protected:
    void SetUp() override
    {
        clean();
        ::mkdir(dir.c_str(), 0700);
        paths.pending_dir = dir + "/pending";
        paths.cert_path = dir + "/client.pem";
        paths.key_path = dir + "/client.key";
    }
    void TearDown() override { clean(); }

    void clean()
    {
        // Sem `rm -rf`: só o que este teste cria.
        for (const char* name : {"client.pem", "client.key", "client.pem.prev", "client.key.prev"}) {
            std::remove((dir + "/" + name).c_str());
        }
        for (const std::string& id : {kRequest, std::string("0190f7a2-0000-7b3a-9f00-1234567890ab")}) {
            std::remove((dir + "/pending/" + id + ".key").c_str());
            std::remove((dir + "/pending/" + id + ".csr").c_str());
        }
        ::rmdir((dir + "/pending").c_str());
        ::rmdir(dir.c_str());
    }

    // Por processo: o ctest roda os testes em paralelo, um processo cada.
    std::string dir = std::string(DEXTRO_IOT_TEST_DIR) + "/iot-certs-" + std::to_string(::getpid());
    CertificatePaths paths;
    TestCa ca;
};

std::string subject_cn(const std::string& csr_pem)
{
    BIO* in = BIO_new_mem_buf(csr_pem.data(), static_cast<int>(csr_pem.size()));
    X509_REQ* req = PEM_read_bio_X509_REQ(in, nullptr, nullptr, nullptr);
    BIO_free(in);
    if (req == nullptr) {
        return {};
    }
    char cn[128] = {};
    X509_NAME_get_text_by_NID(X509_REQ_get_subject_name(req), NID_commonName, cn, sizeof cn);
    EVP_PKEY* pub = X509_REQ_get_pubkey(req);
    const bool p256 = EVP_PKEY_is_a(pub, "EC") == 1 && X509_REQ_verify(req, pub) == 1;
    EVP_PKEY_free(pub);
    X509_REQ_free(req);
    return p256 ? std::string(cn) : std::string("<não é EC válida>");
}

mode_t mode_of(const std::string& path)
{
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 ? (st.st_mode & 0777) : 0;
}

}  // namespace

TEST_F(CertificateStore, CsrLevaONumeroDeSerieEChaveEC)
{
    FileCertificateStore store(kSerial, paths);
    std::string csr;
    ASSERT_EQ(store.request_csr(kRequest, csr), Result::Ok);
    EXPECT_EQ(csr.rfind("-----BEGIN CERTIFICATE REQUEST-----", 0), 0U);
    EXPECT_EQ(subject_cn(csr), kSerial);
    EXPECT_EQ(mode_of(paths.pending_dir + "/" + kRequest + ".key"), 0600U)
        << "a chave privada tem que ser só do daemon";
}

TEST_F(CertificateStore, MesmoPedidoDevolveAMesmaCsr)
{
    FileCertificateStore store(kSerial, paths);
    std::string first;
    std::string again;
    ASSERT_EQ(store.request_csr(kRequest, first), Result::Ok);
    ASSERT_EQ(store.request_csr(kRequest, again), Result::Ok);
    EXPECT_EQ(first, again);
}

TEST_F(CertificateStore, PedidoComCaminhoNoIdERecusado)
{
    FileCertificateStore store(kSerial, paths);
    std::string csr;
    EXPECT_NE(store.request_csr("../../etc/passwd", csr), Result::Ok);
}

TEST_F(CertificateStore, InstalaOCertificadoDaChavePendente)
{
    FileCertificateStore store(kSerial, paths);
    std::string csr;
    ASSERT_EQ(store.request_csr(kRequest, csr), Result::Ok);
    ASSERT_TRUE(dextro::iot::write_file_atomic(paths.cert_path, "velho", 0644));

    std::string fingerprint;
    ASSERT_EQ(store.install(ca.sign(csr), ca.chain(), fingerprint), Result::Ok);
    EXPECT_EQ(fingerprint.size(), 64U);

    std::string installed;
    ASSERT_TRUE(dextro::iot::read_file(paths.cert_path, installed));
    EXPECT_NE(installed.find(ca.chain()), std::string::npos) << "a cadeia vai junto";
    EXPECT_EQ(mode_of(paths.key_path), 0600U);

    std::string previous;
    ASSERT_TRUE(dextro::iot::read_file(paths.cert_path + ".prev", previous));
    EXPECT_EQ(previous, "velho");

    EXPECT_NE(::access((paths.pending_dir + "/" + kRequest + ".key").c_str(), F_OK), 0)
        << "instalado, o pedido pendente sai";
}

TEST_F(CertificateStore, CertificadoDeOutraChaveEMismatch)
{
    FileCertificateStore store(kSerial, paths);
    std::string ours;
    ASSERT_EQ(store.request_csr(kRequest, ours), Result::Ok);

    // Uma CSR de outro store, com outra chave, assinada pela mesma CA.
    CertificatePaths elsewhere = paths;
    elsewhere.pending_dir = dir + "/pending";
    FileCertificateStore other(kSerial, elsewhere);
    std::string theirs;
    ASSERT_EQ(other.request_csr("0190f7a2-0000-7b3a-9f00-1234567890ab", theirs), Result::Ok);
    std::remove((dir + "/pending/0190f7a2-0000-7b3a-9f00-1234567890ab.key").c_str());

    std::string fingerprint;
    EXPECT_EQ(store.install(ca.sign(theirs), ca.chain(), fingerprint), Result::Mismatch);
    EXPECT_NE(::access(paths.cert_path.c_str(), F_OK), 0) << "nada gravado";
}

TEST_F(CertificateStore, PemIlegivelEInvalid)
{
    FileCertificateStore store(kSerial, paths);
    std::string fingerprint;
    EXPECT_EQ(store.install("não é pem", "", fingerprint), Result::Invalid);
}
