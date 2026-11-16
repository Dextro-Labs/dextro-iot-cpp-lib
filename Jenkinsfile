// Build no Jenkins (dextro-pipeline). O GitHub Actions saiu: todo build roda
// no Jenkins desde 26/09/2026.
//
// Os submodules (MQTT-C e nlohmann-json) nos commits fixados pelo repositório,
// e não o master do MQTT-C: o ssat-traffic-controller compila o MQTT-C deste
// caminho e deste commit. O agente precisa de libssl-dev (OpenSSL 3); o
// GoogleTest vem do sistema ou por FetchContent.
@Library('dextro-pipeline') _

dextroLib(stack: 'cmake', pre: 'git submodule update --init --recursive', cmake: '-D BUILD_TESTS=ON -D BUILD_EXAMPLES=ON')
