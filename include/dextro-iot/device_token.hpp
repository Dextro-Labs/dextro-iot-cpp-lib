#pragma once

#include <string>

/// O token `dxd_` do device: a credencial que o `artifact-download` do
/// dextrolabs-device pede no header `device-auth` (download do artefato do
/// OTA, fora do MQTT).
///
/// Onde ele fica é do produto — o pacote do provisionamento de campo grava o
/// arquivo, e alguns produtos também o repetem num `.ini`. Por isso os
/// caminhos são da aplicação:
///
///   1. `file`: o arquivo só com o token (0600), primeira linha;
///   2. se não houver arquivo (ou estiver vazio), `ini_path`, seção
///      `ini_section`, chave `ini_key` (`;` e `#` são comentário).
///
/// No controlador SSAT: `{"/data/ssat/device-token", "/data/ssat.ini",
/// "device", "deviceAuth"}`.
///
/// **Nunca logar o valor.** A função não loga nada.
///
/// Origem: `src/ota/device_token.cpp` do ssat-ota-agent (`origin/devel`
/// 76aa940), com os caminhos fixos virando parâmetro.
namespace dextro::iot {

class DeviceTokenSource {
public:
    std::string file;
    /// Vazio = sem `.ini` de reserva.
    std::string ini_path;
    std::string ini_section = "device";
    std::string ini_key = "deviceAuth";
};

/// O token, sem espaços nas pontas; vazio quando nenhum dos dois o tem.
std::string read_device_token(const DeviceTokenSource& source);

}  // namespace dextro::iot
