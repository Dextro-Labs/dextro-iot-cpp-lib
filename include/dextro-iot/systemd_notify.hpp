#pragma once

#include <cstdint>
#include <string_view>

/// O protocolo de notificação do systemd (`sd_notify(3)`), sem a libsystemd.
///
/// São um `socket()` e um `sendto()` num datagrama AF_UNIX para o endereço de
/// `$NOTIFY_SOCKET`. Em troca o daemon não ganha uma dependência de link que o
/// build de hospedeiro e o SDK do Yocto nem sempre têm — e o binário compilado
/// no PC e o que roda na placa continuam sendo o mesmo código.
///
/// Fora do systemd (`NOTIFY_SOCKET` ausente) tudo devolve `false` e não faz
/// nada: rodar o daemon no terminal continua funcionando.
///
/// Referência: sd_notify(3) e sd_watchdog_enabled(3) em
/// https://www.freedesktop.org/software/systemd/man/ (o endereço começando por
/// `@` é do espaço abstrato; `WATCHDOG_PID` diz de quem é o watchdog; bater
/// na metade de `WATCHDOG_USEC`).
///
/// Origem: `contracts/ssat/app/notify.{hpp,cpp}` do ssat-ota-agent (cópia do
/// ssat-traffic-controller), mais o `Watchdog` de cadência.
namespace dextro::iot::systemd {

/// Envia um estado cru, `CHAVE=valor`, uma ou mais linhas.
bool notify(std::string_view state) noexcept;

/// `READY=1` + `STATUS=` — o systemd só considera uma unit `Type=notify`
/// ativa depois disto.
bool notify_ready(std::string_view status) noexcept;

/// `STATUS=`: uma linha que aparece no `systemctl status`.
bool notify_status(std::string_view status) noexcept;

/// `STOPPING=1` + `STATUS=`.
bool notify_stopping(std::string_view status) noexcept;

/// `WATCHDOG=1`. Sem custo quando não há watchdog armado.
bool notify_watchdog() noexcept;

/// `WATCHDOG_USEC` da unit (`WatchdogSec=`), ou 0 quando não há watchdog para
/// ESTE processo (`WATCHDOG_PID` de outro processo também dá 0).
std::uint64_t watchdog_usec() noexcept;

/// A cadência do `WATCHDOG=1`: na metade do período, como o sd_watchdog_enabled(3)
/// recomenda. Lê o ambiente uma vez, na construção.
///
/// ```cpp
/// dextro::iot::systemd::Watchdog dog;
/// for (;;) {
///     const auto now = dextro::iot::clock::monotonic_ms();
///     int wait = client.tick(now);
///     const auto pet = dog.tick(now);
///     if (pet >= 0 && pet < wait) wait = static_cast<int>(pet);
///     poll(..., wait);
/// }
/// ```
///
/// O watchdog é do **processo**, não da sessão: alimentar só com a sessão
/// aberta reiniciaria justamente quem pode consertar a rede. Num processo com
/// o `ota::Engine` em outra thread, quem alimenta é o laço principal (ou o
/// `KeepGoing` do `Engine`, se ele for o laço do processo).
class Watchdog {
public:
    Watchdog() noexcept;

    bool enabled() const noexcept { return period_ms_ > 0; }

    /// Manda `WATCHDOG=1` quando está na hora. Devolve em quantos ms quer a
    /// próxima chamada, ou -1 sem watchdog.
    std::int64_t tick(std::int64_t now_ms) noexcept;

private:
    std::int64_t period_ms_ = 0;  ///< metade do WATCHDOG_USEC
    std::int64_t next_ms_ = -1;
};

}  // namespace dextro::iot::systemd
