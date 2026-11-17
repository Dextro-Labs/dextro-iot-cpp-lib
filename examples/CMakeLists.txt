add_executable(iot-example main.cpp)
target_link_libraries(iot-example PRIVATE dextro-iot-cpp)

if(DEXTRO_IOT_WITH_OTA)
    add_executable(ota-agent-example ota_agent.cpp)
    target_link_libraries(ota-agent-example PRIVATE dextro-iot-ota)
endif()
