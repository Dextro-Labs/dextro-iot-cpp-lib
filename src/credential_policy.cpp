#include <dextro-iot/credential_policy.hpp>

#include <algorithm>

namespace dextro::iot {

std::optional<CredentialPolicy::Scope> CredentialPolicy::decide(const Inputs& in)
{
    if (!started_) {
        // A sequência do cliente começa em zero na partida: tudo o que ela
        // tiver até aqui são recusas deste certificado.
        switched(0, in.now_ms);
    }
    if (in.connected) {
        last_connected_ms_ = in.now_ms;
        // A sequência do cliente zera ao conectar; a base acompanha.
        streak_base_ = 0;
        return std::nullopt;
    }

    // O cliente zera a sequência ao conectar e a política zera a base junto;
    // se a sequência ficou abaixo da base por outro caminho, recomeça dela.
    streak_base_ = std::min(streak_base_, in.unauthorized_streak);
    const int refusals = in.unauthorized_streak - streak_base_;

    switch (in.current) {
    case Scope::Operational:
        if (in.has_bootstrap && refusals >= kRefusalsBeforeSwitch) {
            return Scope::Bootstrap;
        }
        return std::nullopt;

    case Scope::Bootstrap: {
        if (!in.has_operational) {
            return std::nullopt;
        }
        const std::int64_t since = std::max(scope_since_ms_, last_connected_ms_);
        if (refusals >= kRefusalsBeforeSwitch || in.now_ms - since >= kBootstrapMaxMs) {
            return Scope::Operational;
        }
        return std::nullopt;
    }
    }
    return std::nullopt;
}

void CredentialPolicy::switched(int unauthorized_streak, std::int64_t now_ms) noexcept
{
    started_ = true;
    streak_base_ = unauthorized_streak;
    scope_since_ms_ = now_ms;
}

}  // namespace dextro::iot
