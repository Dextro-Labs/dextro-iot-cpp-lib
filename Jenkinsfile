// Build no Jenkins (dextro-pipeline). O GitHub Actions saiu: todo build roda
// no Jenkins desde 26/09/2026.
@Library('dextro-pipeline') _

dextroLib(stack: 'cmake', pre: 'git clone --depth 1 https://github.com/LiamBindle/MQTT-C.git external/mqtt-c', cmake: '-D BUILD_TESTS=ON -D BUILD_EXAMPLES=ON')
