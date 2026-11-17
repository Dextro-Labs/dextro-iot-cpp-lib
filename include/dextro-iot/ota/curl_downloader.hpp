#pragma once

#include <dextro-iot/ota/ports.hpp>

#include <string>

/// O download do artefato por libcurl (`artifact-download` do
/// dextrolabs-device).
///
///   - só HTTPS, com a CA do sistema (ou `ca_file`), sem seguir
///     redirecionamento: o token vai no header `device-auth` e não pode vazar
///     para outro host;
///   - retoma por `Range` o que já está no `.part`; um 200 no lugar do 206
///     recomeça do zero; 416 apaga o `.part`;
///   - 401/403 → `AUTH_REJECTED` e 404 → `ARTIFACT_NOT_FOUND`, sem retry;
///     mais bytes que o `expected_size` → `SIZE_MISMATCH` (retentável pelo
///     backend);
///   - parado 60 s abaixo de 1 KiB/s é conexão morta (o 4G some sem RST);
///   - `keep_going` é chamado no progresso: `false` para e devolve `Stopped`,
///     com o `.part` guardado.
///
/// Origem: `src/ota/curl_downloader.cpp` do ssat-ota-agent (`origin/devel`
/// 76aa940).
namespace dextro::iot::ota {

class CurlDownloader final : public Downloader {
public:
    class Options {
    public:
        std::string user_agent = "dextro-iot-ota";
        /// PEM da CA; vazio = a do sistema (a do libcurl/OpenSSL).
        std::string ca_file;
        long connect_timeout_s = 30;
        long low_speed_limit = 1024;  ///< bytes/s
        long low_speed_time_s = 60;
    };

    CurlDownloader();
    explicit CurlDownloader(Options options);

    DownloadResult fetch(const DownloadRequest& request, const KeepGoing& keep_going) override;

private:
    Options options_;
};

}  // namespace dextro::iot::ota
