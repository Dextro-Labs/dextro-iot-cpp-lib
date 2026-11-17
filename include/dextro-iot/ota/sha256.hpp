#pragma once

#include <functional>
#include <optional>
#include <string>

namespace dextro::iot::ota {

/// sha256 do arquivo, em hex minúsculo, lido em blocos de 256 KiB (OpenSSL
/// EVP). `keep_going` é chamado entre blocos (é onde o laço alimenta o
/// watchdog); devolvendo `false`, a leitura para e o resultado é vazio. Vazio
/// também se não deu para ler.
///
/// Origem: `src/ota/sha256.cpp` do ssat-ota-agent (`origin/devel` 76aa940).
std::optional<std::string> sha256_file(const std::string& path,
                                       const std::function<bool()>& keep_going = {});

}  // namespace dextro::iot::ota
