cmake_minimum_required(VERSION 3.16)
project(dextro-iot-cpp VERSION 2.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Testes e exemplos ligados só quando este projeto é o de cima: consumido como
# subdiretório (ou no Yocto, sem rede para o FetchContent do GoogleTest), não
# é papel do build de quem consome construir os testes de uma dependência.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(DEXTRO_IOT_TOP_LEVEL ON)
else()
    set(DEXTRO_IOT_TOP_LEVEL OFF)
endif()
option(BUILD_TESTS "Build the unit tests (GoogleTest)" ${DEXTRO_IOT_TOP_LEVEL})
option(BUILD_EXAMPLES "Build the example application" ${DEXTRO_IOT_TOP_LEVEL})
option(DEXTRO_IOT_WERROR "Treat warnings as errors" OFF)

# --------------------------------------------------------------- avisos ---
# Alvo de interface em vez de flags globais: o MQTT-C e o nlohmann não são
# nosso código e não devem ser compilados sob as nossas regras. As mesmas
# flags com que estes arquivos eram compilados no ssat-traffic-controller.
add_library(dextro_iot_warnings INTERFACE)
target_compile_options(dextro_iot_warnings INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wdouble-promotion
    $<$<BOOL:${DEXTRO_IOT_WERROR}>:-Werror>)

# ----------------------------------------------------------------- JSON ---
# O payload do Dextro IoT é JSON. O do sistema quando houver; senão o do
# submodule (o Yocto builda sem rede).
find_package(nlohmann_json 3.2 QUIET)
if(NOT TARGET nlohmann_json::nlohmann_json)
    if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann-json/single_include/nlohmann/json.hpp)
        message(FATAL_ERROR
            "external/nlohmann-json está vazio e o nlohmann_json do sistema não foi achado.\n"
            "  git submodule update --init --recursive")
    endif()
    add_library(dextro_iot_json INTERFACE)
    target_include_directories(dextro_iot_json SYSTEM INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann-json/single_include>)
    add_library(nlohmann_json::nlohmann_json ALIAS dextro_iot_json)
endif()

# mTLS (OpenSSL 3), a chave e a CSR do certificado operacional.
find_package(OpenSSL 3 REQUIRED)
find_package(Threads REQUIRED)
# O resolvedor assíncrono (getaddrinfo_a). Na glibc >= 2.34 ele mora na libc e
# a libanl é só um stub; nas anteriores, é nela.
find_library(DEXTRO_IOT_ANL_LIBRARY anl)

# --------------------------------------------------------------- MQTT-C ---
# O MQTT-C com socket NOSSO (MQTT_USE_CUSTOM_SOCKET_HANDLE): o PAL dele usa
# send() sem MSG_NOSIGNAL — um SIGPIPE mataria o processo —, e o TLS pronto
# dele (MQTT_USE_BIO) gira em laço a 100% de CPU com o buffer cheio num link
# parado. O PAL de verdade é src/session/mqtt_pal.cpp, compilado DENTRO deste
# alvo: o mqtt.c o chama, e numa lib estática separada ele só seria puxado se
# alguém antes o referenciasse.
#
# O mqtt_pal.h não tem ponto de extensão para o tipo do socket; o `-include` é
# o jeito que ele mesmo pede. PUBLIC: todo mundo que vê <mqtt.h> tem que ver o
# mesmo tipo. Quem consome a biblioteca NÃO vê <mqtt.h> (nenhum header público
# o inclui), e por isso a ligação com este alvo é PRIVATE.
#
# O submodule fica em external/mqtt-c, no mesmo commit: o ssat-traffic-controller
# compila o MQTT-C dele a partir deste caminho.
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/external/mqtt-c/src/mqtt.c)
    message(FATAL_ERROR
        "external/mqtt-c está vazio.\n"
        "  git submodule update --init --recursive")
endif()
set(DEXTRO_IOT_MQTT_PAL_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/src/session/mqtt_pal_custom.h)
add_library(dextro_iot_mqttc STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/external/mqtt-c/src/mqtt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/external/mqtt-c/src/mqtt_pal.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/session/mqtt_pal.cpp)
target_include_directories(dextro_iot_mqttc SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/external/mqtt-c/include)
target_include_directories(dextro_iot_mqttc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(dextro_iot_mqttc PUBLIC MQTT_USE_CUSTOM_SOCKET_HANDLE)
# O PAL com relógio monotônico no lugar do time(NULL): ver
# src/session/mqttc_pal.h. PUBLIC pelo mesmo motivo do tipo do socket — quem
# vê <mqtt.h> vê o mesmo PAL. O `src/` também vira PUBLIC: o mqtt.h resolve o
# nome do arquivo pelo caminho de include.
target_compile_definitions(dextro_iot_mqttc PUBLIC MQTTC_PAL_FILE=session/mqttc_pal.h)
target_include_directories(dextro_iot_mqttc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(dextro_iot_mqttc PUBLIC "SHELL:-include ${DEXTRO_IOT_MQTT_PAL_HEADER}")
set_target_properties(dextro_iot_mqttc PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)

# ------------------------------------------------------------ biblioteca ---
add_library(dextro-iot-cpp STATIC
    src/atomic_file.cpp
    src/clock.cpp
    src/event_dispatcher.cpp
    src/iot_client.cpp
    src/keepalive.cpp
    src/log.cpp
    src/certificates/certificate_store.cpp
    src/procedures/envelope.cpp
    src/procedures/procedure.cpp
    src/procedures/procedure_table.cpp
    src/procedures/remote_call.cpp
    src/procedures/rescode.cpp
    src/procedures/ops/certificate_install.cpp
    src/procedures/ops/certificate_request.cpp
    src/procedures/ops/get_status.cpp
    src/procedures/ops/list_procedures.cpp
    src/procedures/ops/outbox_notification.cpp
    src/procedures/ops/settings_read.cpp
    src/procedures/ops/settings_write.cpp
    src/session/backoff.cpp
    src/session/link_monitor.cpp
    src/session/mqtt_session.cpp
    src/session/tls_transport.cpp)
add_library(dextro::iot ALIAS dextro-iot-cpp)
set_target_properties(dextro-iot-cpp PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(dextro-iot-cpp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(dextro-iot-cpp
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE dextro_iot_mqttc OpenSSL::SSL OpenSSL::Crypto Threads::Threads dextro_iot_warnings)
if(DEXTRO_IOT_ANL_LIBRARY)
    target_link_libraries(dextro-iot-cpp PRIVATE ${DEXTRO_IOT_ANL_LIBRARY})
endif()

include(GNUInstallDirs)
install(TARGETS dextro-iot-cpp dextro_iot_mqttc ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/dextro-iot DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
