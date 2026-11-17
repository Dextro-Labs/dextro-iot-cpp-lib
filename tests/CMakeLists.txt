# Preferir o GoogleTest do sistema. O FetchContent é a rede de segurança para o
# hospedeiro que não o tem (o mesmo arranjo do ssat-traffic-controller).
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    message(STATUS "GoogleTest não encontrado no sistema; buscando via FetchContent")
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()
include(GoogleTest)

add_executable(dextro_iot_tests
    # A sessão MQTT contra um broker em memória: uma queda por teste.
    iot_session_test.cpp
    iot_envelope_test.cpp
    iot_remote_call_test.cpp
    iot_procedure_table_test.cpp
    iot_procedures_test.cpp
    # O certificado com OpenSSL contra um diretório temporário.
    iot_certificate_store_test.cpp
    # O TLS contra um servidor TLS de verdade em 127.0.0.1, com CA de
    # bancada: mTLS, nome do servidor, recusas e SIGPIPE.
    iot_tls_transport_test.cpp
    keepalive_test.cpp
    event_dispatcher_test.cpp
    # ota-notification, o inbox e o chaveiro por arquivo.
    iot_ota_notification_test.cpp
    credential_policy_test.cpp
    # O token dxd_ e o sd_notify contra um socket AF_UNIX de verdade.
    device_support_test.cpp)

# O módulo ota: a Engine contra portas falsas e as peças (rauc/systemctl de
# mentira em shell, sem rede).
if(DEXTRO_IOT_WITH_OTA)
    target_sources(dextro_iot_tests PRIVATE
        ota_engine_test.cpp
        ota_parts_test.cpp)
    target_link_libraries(dextro_iot_tests PRIVATE dextro-iot-ota)
endif()

# Os testes veem os internos: o da sessão inclui <mqtt.h>.
target_link_libraries(dextro_iot_tests PRIVATE
    dextro-iot-cpp
    dextro_iot_mqttc
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    GTest::gtest_main)
target_compile_definitions(dextro_iot_tests PRIVATE
    DEXTRO_IOT_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}")

gtest_discover_tests(dextro_iot_tests DISCOVERY_TIMEOUT 60)
