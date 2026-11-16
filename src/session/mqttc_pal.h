#ifndef DEXTRO_IOT_MQTTC_PAL_H
#define DEXTRO_IOT_MQTTC_PAL_H
/* O PAL do MQTT-C, com UMA diferença: o relógio.
 *
 * Entra no lugar do <mqtt_pal.h> pelo ponto de extensão do próprio mqtt.h
 * (MQTTC_PAL_FILE; ver o dextro_iot_mqttc no CMakeLists raiz). Tudo o mais vem do
 * original, incluído aqui mesmo.
 *
 * O original define MQTT_PAL_TIME() como time(NULL), relógio de parede, e o
 * MQTT-C mede com ele o reenvio de QoS 1 (response_timeout) e o tempo de
 * resposta. Esta placa não tem RTC: sobe com a hora do piso salvo e o GNSS/NTP
 * a acerta depois — às vezes um ano para frente. Salto para frente: tudo que
 * está em voo parece vencido e é reenviado de uma vez. Salto para trás: o
 * reenvio nunca chega, e um PUBLISH QoS 1 sem PUBACK fica parado até a sessão
 * cair. O keepalive já era nosso e monotônico (mqtt_session.cpp); com isto o
 * MQTT-C inteiro passa a medir no relógio que não salta.
 *
 * C puro: o mqtt.c é compilado como C99. */
#include <mqtt_pal.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Segundos no relógio monotônico (CLOCK_MONOTONIC). mqtt_pal.cpp. */
mqtt_pal_time_t dextro_iot_mqtt_pal_time(void);

#ifdef __cplusplus
}
#endif

#undef MQTT_PAL_TIME
#define MQTT_PAL_TIME() dextro_iot_mqtt_pal_time()

#endif
