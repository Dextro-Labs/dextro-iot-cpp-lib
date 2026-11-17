// Build no Jenkins (dextro-pipeline). O GitHub Actions saiu: todo build roda
// no Jenkins desde 26/09/2026.
//
// Os submodules (MQTT-C e nlohmann-json) nos commits fixados pelo repositório,
// e não o master do MQTT-C: o ssat-traffic-controller compila o MQTT-C deste
// caminho e deste commit. O agente precisa de libssl-dev (OpenSSL 3) e de
// libcurl4-openssl-dev (módulo ota, forçado ON para não sair do CI calado); o
// GoogleTest vem do sistema ou por FetchContent.
@Library('dextro-pipeline') _

// O stack cmake do dextroLib instala só cmake/ninja/build-essential/git
// (dextro-infra dextroLib.groovy): o OpenSSL 3 e a libcurl de desenvolvimento
// vêm pelo apt.
dextroLib(stack: 'cmake', apt: 'libssl-dev libcurl4-openssl-dev', pre: 'git submodule update --init --recursive', cmake: '-D BUILD_TESTS=ON -D BUILD_EXAMPLES=ON -D DEXTRO_IOT_WITH_OTA=ON')
