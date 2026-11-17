#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/// Arquivo e pasta, do jeito que uma atualização precisa: gravação atômica
/// (tmp + fsync + rename + fsync da pasta), e nada de exceção.
///
/// Origem: `src/ota/fsutil.cpp` do ssat-ota-agent (`origin/devel` 76aa940).
namespace dextro::iot::ota::fs {

/// `mkdir -p`. `true` se a pasta existe no fim.
bool make_dirs(const std::string& dir) noexcept;

/// `<path>.tmp`, fsync, rename sobre `path`, fsync da pasta (0644). Uma queda
/// de energia deixa o arquivo antigo ou o novo, nunca metade.
bool write_atomic(const std::string& path, std::string_view content) noexcept;

/// Lê o arquivo inteiro (até `max_bytes`). `false` se não existe, não dá para
/// ler ou é maior que o teto.
bool read_file(const std::string& path, std::string& out,
               std::size_t max_bytes = 1024 * 1024) noexcept;

/// Tamanho em bytes, ou -1 se não existe.
std::int64_t file_size(const std::string& path) noexcept;

bool exists(const std::string& path) noexcept;

/// `true` se removeu ou se já não existia.
bool remove_file(const std::string& path) noexcept;

/// Os arquivos de `dir` terminados em `suffix` (sem os ocultos), em ordem
/// alfabética, só o nome.
std::vector<std::string> list(const std::string& dir, std::string_view suffix);

}  // namespace dextro::iot::ota::fs
