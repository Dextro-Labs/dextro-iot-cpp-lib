#pragma once

#include <string>
#include <string_view>
#include <sys/types.h>

/// Gravar um arquivo inteiro de uma vez: `.tmp`, `fsync`, `rename`, `fsync` do
/// diretório.
///
/// É o que o `drivers/link/state_file.cpp` do ssat-traffic-controller faz para o enlace, aqui
/// para o que a nuvem entrega — certificado, chave, manifesto do OTA. Uma queda
/// de energia no meio deixa o arquivo antigo ou o novo, nunca metade de um PEM:
/// e um PEM pela metade na chave do mTLS é um controlador que não conecta mais
/// e que só se recupera com alguém no poste.
namespace dextro::iot {

/// `mode` é aplicado com `fchmod`, para um umask herdado do systemd não mudar
/// o que foi pedido (chave é 0600, e tem que ser 0600).
bool write_file_atomic(const std::string& path, std::string_view content, mode_t mode) noexcept;

/// Lê o arquivo inteiro. `false` quando não existe ou não dá para ler.
bool read_file(const std::string& path, std::string& out);

}  // namespace dextro::iot
