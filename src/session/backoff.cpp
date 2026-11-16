#include <dextro-iot/session/backoff.hpp>

#include <algorithm>

namespace dextro::iot::session {

std::int64_t Backoff::next_delay() noexcept
{
    // Teto calculado antes de multiplicar: 2^failures estoura muito antes de
    // alguém esperar tanto, e um `int64` negativo aqui seria reconexão
    // imediata para sempre.
    std::int64_t ceiling = kInitialMs;
    for (int i = 0; i < failures_ && ceiling < kCapMs; ++i) {
        ceiling *= 2;
    }
    ceiling = std::min(ceiling, kCapMs);
    if (failures_ < 64) {
        ++failures_;
    }

    // Full jitter: uniforme em [piso, teto]. O piso de meio segundo evita que
    // um sorteio perto de zero vire tentativa colada na anterior.
    constexpr std::int64_t kFloorMs = 500;
    double r = random_ != nullptr ? random_(state_) : 0.5;
    r = std::clamp(r, 0.0, 1.0);
    const auto span = static_cast<double>(ceiling - kFloorMs);
    return kFloorMs + static_cast<std::int64_t>(r * span);
}

}  // namespace dextro::iot::session
