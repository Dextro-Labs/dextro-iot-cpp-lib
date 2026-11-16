#ifndef DEXTRO_IOT_MQTT_PAL_CUSTOM_H
#define DEXTRO_IOT_MQTT_PAL_CUSTOM_H

/* O "socket" que o MQTT-C enxerga: um ponteiro para o nosso elo com o
 * Transport (src/session/mqtt_pal.cpp).
 *
 * Este arquivo entra por `-include` em toda unidade que vê <mqtt.h> — é o que
 * o MQTT-C pede para MQTT_USE_CUSTOM_SOCKET_HANDLE, porque o mqtt_pal.h dele
 * não tem outro ponto de extensão. Ver o dextro_iot_mqttc no CMakeLists raiz.
 *
 * C puro: o mqtt.c é compilado como C99. */

struct dextro_iot_mqtt_link;
typedef struct dextro_iot_mqtt_link* mqtt_pal_socket_handle;

#endif
