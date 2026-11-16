#pragma once

/// O log da biblioteca, com um gancho para a aplicação.
///
/// A biblioteca não escolhe destino de log: quem a usa liga o seu com
/// `set_sink()` (journal, plog, Qt, o que for). Sem gancho, cada linha sai no
/// stderr com o prefixo `<N>` de `sd-daemon(3)` — é assim que o journald
/// descobre a severidade de um processo que não linka a libsystemd, e o
/// `journalctl -p err` continua servindo.
///
/// A API é **printf**, e não stream, de propósito: o `__attribute__((format))`
/// faz o compilador conferir os argumentos contra o formato.
///
/// Seguro de chamar de qualquer thread: o nível é atômico e o gancho é chamado
/// sob um mutex, uma linha por vez.
///
/// Origem: `providers/log/log.hpp` do ssat-platform, sem a plog por baixo.
namespace dextro::iot::log {

/// Do mais grave para o mais tagarela. A ordem importa: o filtro é `<=`.
enum class Level : int {
    Error = 0,  ///< `<3>`
    Warn = 1,   ///< `<4>`
    Info = 2,   ///< `<6>`
    Debug = 3,  ///< `<7>`
};

/// O destino de uma linha já formatada (sem `\n` no fim). `user` é o ponteiro
/// dado em `set_sink()`. Não pode lançar.
using Sink = void (*)(Level level, const char* line, void* user);

/// Troca o destino. `nullptr` volta para o stderr com prefixo `<N>`.
void set_sink(Sink sink, void* user = nullptr) noexcept;

/// O nível mínimo que sai. Padrão: `Info`.
void set_level(Level level) noexcept;
Level level() noexcept;
bool enabled(Level level) noexcept;

/// Escreve uma linha. Nunca lança.
void write(Level level, const char* format, ...) noexcept __attribute__((format(printf, 2, 3)));

}  // namespace dextro::iot::log

#define DEXTRO_IOT_ERR(...) ::dextro::iot::log::write(::dextro::iot::log::Level::Error, __VA_ARGS__)
#define DEXTRO_IOT_WARN(...) ::dextro::iot::log::write(::dextro::iot::log::Level::Warn, __VA_ARGS__)
#define DEXTRO_IOT_INFO(...) ::dextro::iot::log::write(::dextro::iot::log::Level::Info, __VA_ARGS__)
#define DEXTRO_IOT_DEBUG(...) ::dextro::iot::log::write(::dextro::iot::log::Level::Debug, __VA_ARGS__)
