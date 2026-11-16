#pragma once

#include <dextro-iot/session/transport.hpp>

#include <cstdint>

/// O elo entre o MQTT-C e o `Transport`: o que o PAL customizado recebe como
/// "socket" (ver `mqtt_pal_custom.h`).
///
/// Além de repassar os bytes, **carimba o tempo** de cada byte que entra e
/// sai, no relógio monotônico que a sessão atualiza antes de cada `mqtt_sync`.
/// O MQTT-C mede tudo com `time(NULL)` — relógio de parede, que nesta placa
/// salta quando o GNSS ou o NTP acertam a hora. A detecção de meia-conexão e o
/// keepalive da sessão usam estes carimbos, nunca os do MQTT-C.
///
/// E espia os quatro primeiros bytes da conexão, que são sempre o CONNACK: o
/// MQTT-C não guarda o código de recusa, e "não autorizado" (5) e "servidor
/// indisponível" (3) pedem reações opostas.
struct dextro_iot_mqtt_link {
    dextro::iot::session::Transport* transport = nullptr;
    std::int64_t now_ms = 0;
    std::int64_t last_rx_ms = 0;
    std::int64_t last_tx_ms = 0;
    /// Leituras com dado desde a abertura. É o que diz "chegou algo depois do
    /// PING" — e não o carimbo de tempo: com o broker perto (LAN, docker, EMQX
    /// no mesmo cluster) o PINGRESP chega no MESMO milissegundo em que o PING
    /// saiu, `last_rx_ms > ping_sent_ms` nunca vale e a sessão caía como
    /// meia-conexão 10 s depois, com o broker respondendo normalmente.
    std::uint64_t rx_events = 0;
    bool dead = false;

    unsigned char head[4] = {};
    int head_size = 0;

    /// O código de retorno do CONNACK, ou -1 enquanto não chegou.
    int connack_code() const noexcept
    {
        return head_size >= 4 && head[0] == 0x20 && head[1] == 0x02 ? head[3] : -1;
    }

    void reset(dextro::iot::session::Transport* t, std::int64_t now) noexcept
    {
        transport = t;
        now_ms = now;
        last_rx_ms = now;
        last_tx_ms = now;
        rx_events = 0;
        dead = false;
        head_size = 0;
    }
};
