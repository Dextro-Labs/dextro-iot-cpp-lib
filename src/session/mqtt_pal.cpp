// O PAL do MQTT-C sobre o nosso Transport (MQTT_USE_CUSTOM_SOCKET_HANDLE).
//
// O contrato é o do mqtt_pal.h: devolver quantos bytes passaram — parcial é
// normal, o MQTT-C guarda o deslocamento —, 0 quando é para tentar depois, e
// um MQTTErrors negativo quando o fio morreu. Nunca bloquear.

#include <dextro-iot/session/mqtt_link.hpp>

#include <mqtt.h>

#include <time.h>

// O relógio do MQTT-C (ver mqttc_pal.h): monotônico, em segundos.
extern "C" mqtt_pal_time_t dextro_iot_mqtt_pal_time(void)
{
    timespec ts{};
    (void)::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<mqtt_pal_time_t>(ts.tv_sec);
}

extern "C" ssize_t mqtt_pal_sendall(mqtt_pal_socket_handle link, const void* buf, size_t len,
                                    int flags)
{
    (void)flags;
    if (link == nullptr || link->transport == nullptr || link->dead) {
        return MQTT_ERROR_SOCKET_ERROR;
    }
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = link->transport->send(static_cast<const char*>(buf) + sent, len - sent);
        if (n < 0) {
            link->dead = true;
            break;
        }
        if (n == 0) {
            break;
        }
        sent += static_cast<size_t>(n);
    }
    if (sent > 0) {
        link->last_tx_ms = link->now_ms;
        return static_cast<ssize_t>(sent);
    }
    return link->dead ? MQTT_ERROR_SOCKET_ERROR : 0;
}

extern "C" ssize_t mqtt_pal_recvall(mqtt_pal_socket_handle link, void* buf, size_t bufsz, int flags)
{
    (void)flags;
    if (link == nullptr || link->transport == nullptr || link->dead) {
        return MQTT_ERROR_SOCKET_ERROR;
    }
    auto* out = static_cast<unsigned char*>(buf);
    size_t got = 0;
    while (got < bufsz) {
        const ssize_t n = link->transport->recv(out + got, bufsz - got);
        if (n < 0) {
            link->dead = true;
            break;
        }
        if (n == 0) {
            break;
        }
        // Os primeiros bytes da conexão, para o código do CONNACK.
        for (ssize_t i = 0; i < n && link->head_size < 4; ++i) {
            link->head[link->head_size++] = out[got + static_cast<size_t>(i)];
        }
        got += static_cast<size_t>(n);
    }
    if (got > 0) {
        link->last_rx_ms = link->now_ms;
        ++link->rx_events;
        return static_cast<ssize_t>(got);
    }
    return link->dead ? MQTT_ERROR_SOCKET_ERROR : 0;
}
