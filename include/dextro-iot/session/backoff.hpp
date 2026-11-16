#pragma once

#include <cstdint>

/// O intervalo entre tentativas de reabrir a sessão com o broker.
///
/// Exponencial com *full jitter* (spec §8: "backoff exponencial + jitter, não
/// período fixo"). O jitter não é enfeite: quando a torre de 4G de um bairro
/// volta, todos os controladores do bairro perderam a sessão no mesmo segundo.
/// Com período fixo eles voltariam a bater no broker no mesmo segundo também,
/// para sempre.
///
/// ## Quando zera
///
/// **Só depois de uma sessão estável** (`kStableMs`). Zerar no primeiro
/// CONNACK faria um broker que aceita e derruba em seguida — sessão tomada
/// por outro cliente com o mesmo id, ACL recusando a assinatura — virar um
/// laço apertado de reconexão.
///
/// E zera também quando a **rede muda** (`LinkMonitor`): o motivo de esperar
/// era não ter por onde sair, e esse motivo acabou de deixar de existir.
///
/// Puro: o sorteio vem de fora, pelo `Random`, e o tempo também.
namespace dextro::iot::session {

class Backoff {
public:
    static constexpr std::int64_t kInitialMs = 1000;
    /// O teto. Dois minutos e não mais: a volta imediata quando o link retorna
    /// é do `LinkMonitor`; o teto só cobre o caso em que o link nunca caiu e o
    /// problema é do outro lado (broker fora, DNS do cluster fora).
    static constexpr std::int64_t kCapMs = 120000;
    /// Quanto tempo uma sessão tem que durar para o backoff esquecer as falhas.
    static constexpr std::int64_t kStableMs = 60000;

    /// Um número em [0, 1). Injetado: o teste precisa dos extremos.
    using Random = double (*)(void* state);

    Backoff(Random random, void* state) noexcept : random_(random), state_(state) {}

    /// Registra uma falha e devolve quanto esperar, em ms.
    std::int64_t next_delay() noexcept;

    /// A rede mudou, ou a sessão ficou estável: a próxima espera é a mínima.
    void reset() noexcept { failures_ = 0; }

    int failures() const noexcept { return failures_; }

private:
    Random random_;
    void* state_;
    int failures_ = 0;
};

}  // namespace dextro::iot::session
