// Exemplo do módulo ota: um agente de atualização em processo próprio (como o
// ssat-otad do controlador SSAT), A/B por RAUC.
//
//   ota-agent-example <base> <keyring> <token>
//
//   <base>     a pasta de trabalho na partição de dados (inbox/, state/,
//              download/); o processo do `Client` registra o `ota-notification`
//              com um `InboxOtaSpool(<base>/inbox, ...)`
//   <keyring>  as chaves ed25519 da imagem (`<signingKeyId>.pub`)
//   <token>    o arquivo com o token dxd_ do device
//
// A trilha aqui vai para o stdout só para o exemplo caber num arquivo: num
// device de verdade o `EventSink` grava na fila durável da aplicação (o
// `EventStore` do `EventDispatcher`).
//
// A `Engine` bloqueia (download, sha256, `rauc install`): é por isso que ela
// mora num processo (ou thread) que não é o do laço do `Client`.
#include <dextro-iot/clock.hpp>
#include <dextro-iot/ota/curl_downloader.hpp>
#include <dextro-iot/ota/engine.hpp>
#include <dextro-iot/ota/linux_platform.hpp>
#include <dextro-iot/ota/rauc.hpp>
#include <dextro-iot/systemd_notify.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <thread>

namespace ota = dextro::iot::ota;

namespace {

volatile std::sig_atomic_t g_stop = 0;

class StdoutEvents final : public ota::EventSink {
public:
    bool emit(std::string_view code, const nlohmann::json& args, std::int64_t occurred_ms) override
    {
        std::printf("%lld %.*s %s\n", static_cast<long long>(occurred_ms),
                    static_cast<int>(code.size()), code.data(), args.dump().c_str());
        std::fflush(stdout);
        return true;
    }
};

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::fprintf(stderr, "uso: %s <base> <keyring> <token>\n", argv[0]);
        return 2;
    }
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    std::signal(SIGINT, [](int) { g_stop = 1; });

    ota::LinuxPlatform::Config pc;
    pc.slot_cmdline_key = "rauc.slot=";  // o SSAT usa "ssat.slot=" + slot_upper_case
    pc.token.file = argv[3];
    ota::LinuxPlatform platform(pc);
    ota::CurlDownloader downloader;
    ota::RaucInstaller rauc;
    StdoutEvents events;

    ota::Engine engine(ota::Engine::Paths::under(argv[1], argv[2]), downloader, platform, events);
    engine.add_handler(std::make_unique<ota::RaucHandler>(rauc));

    // O watchdog do processo é alimentado também no meio do download e do
    // `rauc install`, pelo KeepGoing; `false` interrompe o download (o `.part`
    // fica para retomar).
    dextro::iot::systemd::Watchdog dog;
    engine.set_keep_going([&dog] {
        dog.tick(dextro::iot::clock::monotonic_ms());
        return g_stop == 0;
    });
    dextro::iot::systemd::notify_ready("aguardando atualização");

    while (g_stop == 0) {
        std::int64_t wait = engine.tick();
        const std::int64_t pet = dog.tick(dextro::iot::clock::monotonic_ms());
        if (pet >= 0 && pet < wait) {
            wait = pet;
        }
        // Em fatias de 1 s: o SIGTERM não espera o minuto do applyAfter.
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<std::int64_t>(wait, 1000)));
    }
    dextro::iot::systemd::notify_stopping("parando");
    return 0;
}
