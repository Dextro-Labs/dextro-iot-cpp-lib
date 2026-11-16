#pragma once

#include <dextro-iot/procedures/ports.hpp>

#include <string>

/// O certificado operacional do device, em arquivo (SPEC do dextrolabs-device §3.5).
///
/// **Driver**: implementa o port `CertificateStore` sobre OpenSSL 3. A
/// biblioteca não aparece neste header — só PEM em `std::string` —, então
/// quem compõe o daemon não arrasta `<openssl/*>`.
///
/// ## Onde as coisas ficam
///
///   pending_dir/<requestId>.key   chave gerada no certificate-request, 0600
///   pending_dir/<requestId>.csr   a CSR devolvida (reentrega devolve esta)
///   cert_path                     certificado + cadeia, depois do install
///   key_path                      a chave que casou com o certificado, 0600
///   cert_path.prev / key_path.prev   o par anterior, para voltar atrás à mão
///
/// Tudo numa partição que sobrevive à troca de imagem (o `/data` de uma
/// imagem A/B com RAUC ou SWUpdate): o certificado operacional não pode sumir
/// num update. Quem escolhe os caminhos é a aplicação.
///
/// ## O que a CSR leva
///
/// O que o service valida (contracts.md A.4.7): `CN = serialNumber`, EC P-256,
/// sem SAN, e só `keyUsage` e `extendedKeyUsage=clientAuth` como extensões.
/// Qualquer coisa a mais e ele recusa com `CSR_INVALID`.
namespace dextro::iot {

class CertificatePaths {
public:
    std::string pending_dir;
    std::string cert_path;
    std::string key_path;
};

class FileCertificateStore final : public procedures::CertificateStore {
public:
    FileCertificateStore(std::string common_name, CertificatePaths paths);

    Result request_csr(std::string_view request_id, std::string& csr_pem) override;
    Result install(std::string_view cert_pem, std::string_view chain_pem,
                   std::string& fingerprint) override;

private:
    std::string common_name_;
    CertificatePaths paths_;
};

/// SHA-256 (hex) do primeiro certificado do PEM em `pem_path` — o
/// `certFingerprint` do keepalive, que ativa o certificado `pending_install`
/// no service (SPEC do dextrolabs-device §3.5, §4.4). Vazio se não leu.
std::string certificate_fingerprint(const std::string& pem_path);

}  // namespace dextro::iot
