#include <dextro-iot/event_dispatcher.hpp>

#include <dextro-iot/log.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>

namespace dextro::iot {
namespace {

using Json = nlohmann::json;

bool final_reason(const std::string& reason)
{
    return reason == "UNKNOWN_EVENT_TYPE" || reason == "INVALID_ARGS";
}

std::uint32_t seed()
{
    return std::random_device{}();
}

}  // namespace

EventDispatcher::EventDispatcher(RemoteLink& link, EventStore& store, std::int64_t interval_ms,
                                 WallFn wall)
    : link_(link),
      store_(store),
      interval_ms_(interval_ms > 0 ? interval_ms : kDefaultIntervalMs),
      wall_(wall),
      rng_(seed())
{
}

std::string EventDispatcher::encode(const std::vector<StoredEvent>& batch)
{
    Json events = Json::array();
    for (const StoredEvent& e : batch) {
        Json item = {{"seq", e.seq}, {"code", e.code}, {"occurredAt", e.occurred_ms}};
        if (!e.args.empty()) {
            Json args = Json::parse(e.args, nullptr, /*allow_exceptions=*/false);
            if (args.is_object()) {
                item["args"] = std::move(args);
            }
        }
        events.push_back(std::move(item));
    }
    // `replace`: um byte inválido em UTF-8 num `args` livre não pode virar
    // exceção aqui — viraria bloco nenhum, para sempre.
    return Json{{"events", std::move(events)}}.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::int64_t EventDispatcher::next_in(std::int64_t now_ms) const noexcept
{
    if (!link_.connected() || in_flight_) {
        return -1;
    }
    if (!work_.empty() || kicked_ || drain_now_ || retry_in_ms_ >= 0) {
        return 0;
    }
    return std::max<std::int64_t>(next_drain_ms_ - now_ms, 0);
}

void EventDispatcher::end_drain(std::int64_t delay_ms)
{
    work_.clear();
    batches_this_drain_ = 0;
    retry_in_ms_ = std::max<std::int64_t>(delay_ms, 0);
}

void EventDispatcher::tick(std::int64_t now_ms)
{
    const bool connected = link_.connected();
    if (connected && !was_connected_) {
        // §4.5: drenar ao conectar.
        ++generation_;
        in_flight_ = false;
        work_.clear();
        batches_this_drain_ = 0;
        retry_in_ms_ = -1;
        next_drain_ms_ = now_ms;
    }
    was_connected_ = connected;
    if (!connected) {
        // O bloco em voo, se houver, volta 503 pelo `Client`. A fila não
        // perdeu nada: o que não foi confirmado continua lá.
        work_.clear();
        return;
    }
    if (in_flight_) {
        return;
    }
    if (retry_in_ms_ >= 0) {
        next_drain_ms_ = now_ms + retry_in_ms_;
        retry_in_ms_ = -1;
    }
    if (kicked_ || drain_now_) {
        kicked_ = false;
        drain_now_ = false;
        next_drain_ms_ = std::min(next_drain_ms_, now_ms);
    }
    if (work_.empty()) {
        if (now_ms < next_drain_ms_) {
            return;
        }
        if (batches_this_drain_ >= kMaxBatchesPerDrain) {
            batches_this_drain_ = 0;
            next_drain_ms_ = now_ms + std::min<std::int64_t>(interval_ms_, 1000);
            return;
        }
        const std::int64_t wall = wall_ != nullptr ? wall_() : 0;
        std::vector<StoredEvent> batch = store_.due(wall, kMaxBatch);
        if (batch.empty()) {
            batches_this_drain_ = 0;
            std::int64_t delay = interval_ms_;
            if (const auto next = store_.next_touch()) {
                delay = std::clamp<std::int64_t>(*next - wall, 1000, interval_ms_);
            }
            next_drain_ms_ = now_ms + delay;
            return;
        }
        if (batch.size() > kMaxBatch) {
            batch.resize(kMaxBatch);
        }
        work_.push_back(std::move(batch));
    }
    send_next(now_ms);
}

void EventDispatcher::send_next(std::int64_t now_ms)
{
    while (!work_.empty() && !in_flight_ && link_.connected()) {
        std::vector<StoredEvent> batch = std::move(work_.front());
        work_.pop_front();
        if (batch.empty()) {
            continue;
        }
        std::string body = encode(batch);
        if (body.size() > kMaxBatchBytes) {
            if (batch.size() == 1) {
                reject_one(batch.front(), "evento acima de 256 KB");
                continue;
            }
            // Divide aqui, antes de sair: o backend responderia 413 ao bloco.
            const auto mid = batch.begin() + static_cast<std::ptrdiff_t>(batch.size() / 2);
            work_.emplace_front(mid, batch.end());
            work_.emplace_front(batch.begin(), mid);
            continue;
        }

        in_flight_ = true;
        ++batches_this_drain_;
        const std::uint64_t generation = generation_;
        // O `done` pode rodar já, dentro do `call` (503 sem sessão, 413 local).
        link_.call("events-write", body, now_ms, kCallTimeoutMs,
                   [this, generation, batch](const RemoteResult& r) {
                       if (generation != generation_) {
                           return;  // de uma sessão anterior
                       }
                       on_reply(batch, r);
                   });
    }
}

void EventDispatcher::reject_one(const StoredEvent& event, const std::string& why)
{
    store_.reject(event.seq, why);
    ++rejected_;
    DEXTRO_IOT_WARN("dextro-iot: evento seq %lld recusado sozinho (%s): fica fora da fila",
                    static_cast<long long>(event.seq), why.c_str());
}

void EventDispatcher::on_reply(const std::vector<StoredEvent>& batch, const RemoteResult& r)
{
    in_flight_ = false;
    const int rescode = r.rescode;

    if (rescode == 503) {
        // Sem sessão (ou ela caiu com o pedido em voo): não é culpa do evento
        // e não gasta tentativa. A próxima sessão drena.
        end_drain(interval_ms_);
        return;
    }
    if (rescode == 400 || rescode == 413) {
        if (batch.size() == 1) {
            reject_one(batch.front(), "bloco recusado: " + std::to_string(rescode) + " " + r.error);
            drain_now_ = true;
            return;
        }
        // Divide ao meio e manda cada metade: isola o item que não passa.
        const auto mid = batch.begin() + static_cast<std::ptrdiff_t>(batch.size() / 2);
        work_.emplace_front(mid, batch.end());
        work_.emplace_front(batch.begin(), mid);
        return;
    }
    if (rescode < 200 || rescode >= 300) {
        retry_later(batch, std::to_string(rescode) + " " + r.error);
        const std::int64_t wall = wall_ != nullptr ? wall_() : 0;
        std::int64_t delay = interval_ms_;
        if (const auto next = store_.next_touch()) {
            delay = std::clamp<std::int64_t>(*next - wall, 1000, interval_ms_);
        }
        end_drain(delay);
        return;
    }

    const Json p = Json::parse(r.payload_json, nullptr, /*allow_exceptions=*/false);
    const Json body = p.is_object() ? p : Json::object();
    std::set<std::int64_t> retry;
    if (body.contains("rejected") && body["rejected"].is_array()) {
        for (const Json& item : body["rejected"]) {
            if (!item.is_object()) {
                continue;
            }
            const std::int64_t seq = item.value("seq", std::int64_t{0});
            const std::string reason = item.value("reason", std::string{});
            if (final_reason(reason)) {
                store_.reject(seq, reason + ": " + item.value("detail", std::string{}));
                ++rejected_;
                DEXTRO_IOT_WARN("dextro-iot: evento seq %lld recusado pelo backend: %s",
                                static_cast<long long>(seq), reason.c_str());
            } else {
                retry.insert(seq);  // CLOCK_SKEW e o que vier: tenta de novo
            }
        }
    }
    // O que não voltou como recusado (nem para reenviar) foi aceito ou é
    // duplicado: sai da fila, mesmo fora do contíguo do ackSeq.
    std::set<std::int64_t> refused;
    if (body.contains("rejected") && body["rejected"].is_array()) {
        for (const Json& item : body["rejected"]) {
            if (item.is_object()) {
                refused.insert(item.value("seq", std::int64_t{0}));
            }
        }
    }
    std::vector<std::int64_t> done;
    std::vector<StoredEvent> again;
    for (const StoredEvent& e : batch) {
        if (retry.count(e.seq) != 0) {
            again.push_back(e);
        } else if (refused.count(e.seq) == 0) {
            done.push_back(e.seq);
        }
    }
    std::int64_t ack = 0;
    if (body.contains("ackSeq") && body["ackSeq"].is_number_integer()) {
        ack = body["ackSeq"].get<std::int64_t>();
    }
    const std::size_t removed = store_.acknowledge(ack, done);
    sent_ += removed;
    DEXTRO_IOT_DEBUG("dextro-iot: bloco de %zu: %zu resolvido(s), ackSeq %lld", batch.size(),
                     removed, static_cast<long long>(ack));
    if (!again.empty()) {
        retry_later(again, "CLOCK_SKEW");
    }
    if (work_.empty()) {
        drain_now_ = true;  // o próximo bloco, se a fila tiver mais
    }
}

void EventDispatcher::retry_later(const std::vector<StoredEvent>& batch, const std::string& why)
{
    int attempts = 0;
    std::vector<std::int64_t> seqs;
    seqs.reserve(batch.size());
    for (const StoredEvent& e : batch) {
        seqs.push_back(e.seq);
        attempts = std::max(attempts, e.attempts);
    }
    // min(30 s·2^n, 15 min), com jitter entre metade e o valor inteiro.
    std::int64_t delay = kBackoffFirstMs;
    for (int i = 0; i < attempts && delay < kBackoffCapMs; ++i) {
        delay *= 2;
    }
    delay = std::min(delay, kBackoffCapMs);
    std::uniform_int_distribution<std::int64_t> jitter(delay / 2, delay);
    const std::int64_t wall = wall_ != nullptr ? wall_() : 0;
    const std::int64_t next = wall + jitter(rng_);
    store_.reschedule(seqs, next, why, /*count_attempt=*/true);
    DEXTRO_IOT_WARN("dextro-iot: %zu evento(s) de volta para a fila (%s); próxima tentativa em %lld s",
                    seqs.size(), why.c_str(), static_cast<long long>((next - wall) / 1000));
}

}  // namespace dextro::iot
