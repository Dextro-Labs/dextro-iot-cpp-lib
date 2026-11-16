#pragma once

#include <dextro-iot/clock.hpp>
#include <dextro-iot/remote_link.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

/// A fila de eventos do device até o backend, pela remote-procedure
/// `events-write` (SPEC do dextrolabs-device §4.5).
///
/// A biblioteca NÃO guarda evento: a fila é da aplicação, atrás do port
/// `EventStore` (numa aplicação C++ da Dextro, uma tabela do MoleculaEntity).
/// Aqui só mora o que é do protocolo: montar o bloco, mandar, e o que fazer
/// com cada resposta.
namespace dextro::iot {

/// Um evento na fila da aplicação.
class StoredEvent {
public:
    /// u64 monotônico por device, **persistido entre reboots e nunca
    /// reusado** (§4.5). O rowid de uma tabela pode voltar depois de um
    /// delete; um contador próprio, gravado na mesma transação do insert, não.
    std::int64_t seq = 0;
    /// `device_event_type.code` do tipo do device, ou global.
    std::string code;
    /// JSON (objeto) ou vazio. Validado no backend contra o `args_schema`.
    std::string args;
    /// Epoch ms do instante em que aconteceu. Um reenvio DEVE repetir o
    /// mesmo valor: a deduplicação do backend é por (occurredAt, device, seq).
    std::int64_t occurred_ms = 0;
    /// Tentativas que falharam por culpa do evento ou do backend (não conta
    /// "sem sessão"). Base do backoff.
    int attempts = 0;
};

/// O port: a fila durável, implementada pela aplicação. Chamado só da thread
/// do `tick()`; se a aplicação grava eventos de outra thread, a sincronização
/// é dela.
class EventStore {
public:
    virtual ~EventStore() = default;

    /// Os pendentes que já podem tentar (`nextTouch <= now_wall_ms`), em ordem
    /// de `seq`, no máximo `limit`.
    virtual std::vector<StoredEvent> due(std::int64_t now_wall_ms, std::size_t limit) = 0;

    /// Tira da fila os resolvidos: **todo** pendente com `seq <= ack_seq` (o
    /// device PODE descartar tudo até o `ackSeq`, §4.5) e os `seqs` dados
    /// (aceitos e duplicados fora do contíguo). Devolve quantos saíram.
    virtual std::size_t acknowledge(std::int64_t ack_seq, const std::vector<std::int64_t>& seqs) = 0;

    /// Recusado de vez (`UNKNOWN_EVENT_TYPE`, `INVALID_ARGS`, ou isolado num
    /// bloco 400/413): não volta mais para a fila. Guardar para diagnóstico ou
    /// apagar é da aplicação.
    virtual void reject(std::int64_t seq, std::string_view reason) = 0;

    /// Volta para a fila com `nextTouch = next_touch_wall_ms`. `count_attempt`
    /// incrementa `attempts`.
    virtual void reschedule(const std::vector<std::int64_t>& seqs, std::int64_t next_touch_wall_ms,
                            std::string_view why, bool count_attempt) = 0;

    /// O menor `nextTouch` entre os pendentes, ou nada (fila vazia).
    virtual std::optional<std::int64_t> next_touch() = 0;
};

/// O despachante: drena o `EventStore` pelo `events-write`, um bloco em voo
/// por vez, sem bloquear a thread (a resposta chega pelo `tick()` do `Client`).
///
/// ## Quando manda
///
/// Ao conectar (§4.5: "DEVE drenar o buffer ao conectar e depois
/// periodicamente"), a cada `interval_ms`, e logo depois de `kick()` — que a
/// aplicação chama quando grava um evento. Sem sessão, não tenta: o evento
/// fica na fila sem gastar tentativa.
///
/// ## O bloco
///
/// Até `kMaxBatch` (200, `ATRIO_EVENTS_MAX_BATCH`) eventos e `kMaxBatchBytes`
/// (256 KB) de payload. Um bloco acima dos 256 KB é dividido aqui mesmo,
/// antes de sair; um evento sozinho acima disso é recusado localmente (o
/// backend responderia 413 para sempre).
///
/// ## O que faz com a resposta
///
///   - aceitos, duplicados e tudo com `seq <= ackSeq` saem da fila;
///   - `UNKNOWN_EVENT_TYPE` e `INVALID_ARGS` são finais: `reject()`;
///   - `CLOCK_SKEW` (e outro motivo qualquer) volta para a fila com backoff;
///   - 503 (sem sessão, ou ela caiu com o pedido em voo): **sem penalidade** —
///     não é culpa do evento, nada é reagendado nem conta tentativa; a próxima
///     sessão drena;
///   - 400 ou 413 no BLOCO inteiro: o bloco é dividido ao meio até isolar o
///     item que o backend não aceita, que vira recusado;
///   - outro erro (5xx, 429, 504 silêncio): o bloco volta para a fila com
///     `nextTouch = agora + min(30 s·2^n, 15 min)`, com jitter entre metade e
///     o valor inteiro.
///
/// Origem: `src/services/event-manager/event_dispatcher.cpp` do
/// ssat-traffic-controller, que era síncrono sobre o bus de daemons e o
/// `EventStore` em SQLite de lá. Aqui é assíncrono sobre o `RemoteLink`, com a
/// mesma política de resposta.
class EventDispatcher {
public:
    static constexpr std::size_t kMaxBatch = 200;
    static constexpr std::size_t kMaxBatchBytes = 256 * 1024;
    static constexpr std::int64_t kBackoffFirstMs = 30000;
    static constexpr std::int64_t kBackoffCapMs = 15 * 60 * 1000;
    static constexpr std::int64_t kCallTimeoutMs = 30000;
    static constexpr std::int64_t kDefaultIntervalMs = 60000;
    /// Blocos por drenagem: uma fila enorme não monopoliza a sessão (o
    /// keepalive e as device-procedures usam o mesmo fio).
    static constexpr int kMaxBatchesPerDrain = 50;

    using WallFn = std::int64_t (*)();

    EventDispatcher(RemoteLink& link, EventStore& store,
                    std::int64_t interval_ms = kDefaultIntervalMs,
                    WallFn wall = &clock::wall_ms);

    /// Um evento entrou na fila: drenar na próxima volta.
    void kick() noexcept { kicked_ = true; }

    /// Chamar a cada volta, depois do `Client::tick()`.
    void tick(std::int64_t now_ms);

    /// Em quantos ms quer ser chamado de novo (teto do `poll`); -1 = sem
    /// pressa (sem sessão ou com bloco em voo).
    std::int64_t next_in(std::int64_t now_ms) const noexcept;

    /// Eventos que o backend deu por resolvidos (aceitos e duplicados).
    std::uint64_t sent() const noexcept { return sent_; }
    /// Recusados de vez (pelo backend ou por tamanho).
    std::uint64_t rejected() const noexcept { return rejected_; }
    bool in_flight() const noexcept { return in_flight_; }

    /// O payload `{"events":[...]}` de um bloco, em texto. Público para teste.
    static std::string encode(const std::vector<StoredEvent>& batch);

private:
    void send_next(std::int64_t now_ms);
    void on_reply(const std::vector<StoredEvent>& batch, const RemoteResult& r);
    void retry_later(const std::vector<StoredEvent>& batch, const std::string& why);
    void reject_one(const StoredEvent& event, const std::string& why);
    void end_drain(std::int64_t delay_ms);

    RemoteLink& link_;
    EventStore& store_;
    std::int64_t interval_ms_;
    WallFn wall_;

    bool was_connected_ = false;
    bool kicked_ = false;
    bool in_flight_ = false;
    /// Quando a próxima drenagem pode começar (monotônico).
    std::int64_t next_drain_ms_ = 0;
    /// Pedido de "drenar já" vindo de dentro de uma resposta (o `tick` aplica).
    bool drain_now_ = false;
    std::int64_t retry_in_ms_ = -1;
    int batches_this_drain_ = 0;
    /// Os blocos que faltam mandar nesta drenagem: o que a divisão ao meio
    /// produz, o primeiro na frente.
    std::deque<std::vector<StoredEvent>> work_;
    std::uint64_t generation_ = 0;
    std::uint64_t sent_ = 0;
    std::uint64_t rejected_ = 0;
    std::mt19937 rng_;
};

}  // namespace dextro::iot
