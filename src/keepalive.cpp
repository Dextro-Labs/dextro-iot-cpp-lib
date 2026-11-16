#include <dextro-iot/keepalive.hpp>

#include <dextro-iot/log.hpp>

#include <algorithm>
#include <array>
#include <string_view>

namespace dextro::iot {
namespace {

using Json = nlohmann::json;

/// Os campos padrão do `KeepalivePayload` (SPEC §4.4) que a aplicação pode
/// mandar em `Status::extra`. Os outros quatro são da biblioteca.
constexpr std::array<std::string_view, 6> kExtraKeys = {"rssi",  "freeMemBytes", "cpuLoad",
                                                        "tempC", "ip",           "batteryPct"};

}  // namespace

Keepalive::Keepalive(RemoteLink& link, StatusFn status, std::int64_t started_ms, NowFn now)
    : link_(link), status_(std::move(status)), started_ms_(started_ms), now_(now)
{
}

Json Keepalive::payload(std::int64_t now_ms) const
{
    Json out = {{"intervalMs", interval_ms_}, {"uptimeS", (now_ms - started_ms_) / 1000}};
    if (!status_) {
        return out;
    }
    const Status s = status_();
    if (!s.fw_version.empty()) {
        out["fwVersion"] = s.fw_version;
    }
    if (s.settings_version >= 0) {
        out["settingsVersion"] = s.settings_version;
    }
    if (!s.cert_fingerprint.empty()) {
        out["certFingerprint"] = s.cert_fingerprint;
    }
    if (s.extra.is_object()) {
        for (const auto& item : s.extra.items()) {
            if (std::find(kExtraKeys.begin(), kExtraKeys.end(), item.key()) != kExtraKeys.end()) {
                out[item.key()] = item.value();
            } else {
                // `additionalProperties: false`: uma chave a mais seria 400 em
                // TODO keepalive, e o keepalive é a sonda do service.
                DEXTRO_IOT_WARN("dextro-iot: keepalive: campo '%s' fora do §4.4 descartado "
                                "(o que é do tipo vai em custom)",
                                item.key().c_str());
            }
        }
    }
    if (s.custom.is_object() && !s.custom.empty()) {
        out["custom"] = s.custom;
    }
    return out;
}

std::int64_t Keepalive::next_in(std::int64_t now_ms) const noexcept
{
    if (!link_.connected() || in_flight_) {
        return -1;
    }
    return std::max<std::int64_t>(next_ms_ - now_ms, 0);
}

void Keepalive::tick(std::int64_t now_ms)
{
    const bool connected = link_.connected();
    if (connected && !was_connected_) {
        // Keepalive já na abertura: é ele que ativa um certificado recém
        // instalado e que confirma que o service responde nesta sessão.
        ++generation_;
        next_ms_ = now_ms;
        misses_ = 0;
        in_flight_ = false;
    }
    was_connected_ = connected;
    if (!connected || in_flight_ || now_ms < next_ms_) {
        return;
    }
    in_flight_ = true;
    const std::uint64_t generation = generation_;
    link_.call("keepalive", payload(now_ms).dump(), now_ms, 0,
               [this, generation](const RemoteResult& r) {
                   if (generation != generation_) {
                       return;  // de uma sessão anterior
                   }
                   reply(r);
               });
}

void Keepalive::reply(const RemoteResult& r)
{
    in_flight_ = false;
    const std::int64_t now = now_ != nullptr ? now_() : 0;

    // 503 aqui é local: a sessão caiu ou foi reaberta com o pedido em voo.
    // Não diz nada sobre o service; a próxima sessão manda o seu.
    if (r.rescode == 503) {
        return;
    }
    // Qualquer resposta do backend — até um 4xx — prova que o service está
    // do outro lado. Só o silêncio (504) e o 5xx contam como falha.
    if (r.rescode != 504 && r.rescode < 500) {
        misses_ = 0;
        restarts_ = 0;
        if (!r.ok()) {
            DEXTRO_IOT_WARN("dextro-iot: keepalive recusado pelo backend: %d %s", r.rescode,
                            r.error.c_str());
        }
        const Json body = Json::parse(r.payload_json, nullptr, /*allow_exceptions=*/false);
        if (body.is_object() && body.contains("nextIntervalMs") &&
            body["nextIntervalMs"].is_number_integer()) {
            interval_ms_ = std::clamp(body["nextIntervalMs"].get<std::int64_t>(), kMinMs, kMaxMs);
        }
        next_ms_ = now + interval_ms_;
        if (r.ok() && body.is_object() && on_reply_) {
            try {
                on_reply_(body);
            } catch (...) {
                // Quem trata a resposta não derruba a thread.
            }
        }
        return;
    }

    ++misses_;
    const int threshold =
        std::min(kMissesBeforeRestart << std::min(restarts_, 4), kMissesCap);
    DEXTRO_IOT_WARN("dextro-iot: keepalive sem resposta do service (%d %s), %d de %d", r.rescode,
                    r.error.c_str(), misses_, threshold);
    if (misses_ >= threshold && link_.connected()) {
        DEXTRO_IOT_WARN("dextro-iot: o broker responde e o service não: reabrindo a sessão");
        misses_ = 0;
        ++restarts_;
        link_.restart(now);
        return;
    }
    // Tenta de novo logo, sem esperar o intervalo inteiro.
    next_ms_ = now + kMinMs;
}

}  // namespace dextro::iot
