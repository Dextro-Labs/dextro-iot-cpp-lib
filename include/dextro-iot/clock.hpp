#pragma once

#include <cstdint>

/// `CLOCK_MONOTONIC`, e só ele.
///
/// Monotônico e não hora de parede porque o device SINCRONIZA o relógio
/// (GNSS, NTP) depois de ligar — às vezes um ano para frente: um salto de hora
/// não pode virar salto de prazo, de keepalive nem de backoff. Todo `now_ms`
/// que a biblioteca recebe (`tick`, `start`, `call`...) é deste relógio.
///
/// Origem: `providers/clock/monotonic.hpp` do ssat-platform (submodule do
/// ssat-traffic-controller), reduzido ao que a biblioteca usa.
namespace dextro::iot::clock {

/// Nanossegundos desde um instante arbitrário e imóvel. Nunca anda para trás.
std::int64_t monotonic_ns() noexcept;

/// Milissegundos do mesmo relógio — a unidade de todo `now_ms` da biblioteca.
inline std::int64_t monotonic_ms() noexcept
{
    return monotonic_ns() / 1000000;
}

/// O relógio de parede, em ms desde a época — só para carimbar mensagens
/// (`timestamp`, `ts` da presença, `occurredAt`), nunca para medir prazo.
std::int64_t wall_ms() noexcept;

}  // namespace dextro::iot::clock
