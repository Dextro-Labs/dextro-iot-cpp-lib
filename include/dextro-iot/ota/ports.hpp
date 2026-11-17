#pragma once

#include <cstdint>
#include <functional>
#include <string>

/// O que a máquina de estados do OTA pede ao mundo. Cada coisa que toca rede,
/// RAUC, systemd ou o relógio passa por aqui — e é isso que deixa o `Engine`
/// ser testado inteiro sem placa, sem rede e sem `rauc`.
///
/// As implementações de produção: `CurlDownloader` (`ota/curl_downloader.hpp`)
/// e `LinuxPlatform` (`ota/linux_platform.hpp`). A trilha vai pelo
/// `EventSink` (`ota/event_sink.hpp`), da aplicação.
///
/// Origem: `include/ssat/ota/ports.hpp` do ssat-ota-agent (`origin/devel`
/// 76aa940). As portas do SSAT (`PowerModules`, `Coprocessor`) ficaram lá,
/// atrás dos `ArtifactHandler` deles.
namespace dextro::iot::ota {

/// Chamado durante as operações longas (download, sha256, `rauc install`). É
/// onde o laço alimenta o watchdog; `false` = pare (SIGTERM).
using KeepGoing = std::function<bool()>;

class DownloadRequest {
public:
    std::string url;
    std::string token;               ///< vai no header `device-auth`
    std::string path;                ///< o `.part`; o que já existe é retomado (Range)
    std::int64_t expected_size = 0;  ///< 0 = desconhecido; acima disto, aborta
};

class DownloadResult {
public:
    enum class Outcome {
        Ok,         ///< o arquivo está inteiro em `path`
        Transient,  ///< rede, 5xx, prazo: tentar de novo mais tarde
        Permanent,  ///< 401/403/404, tamanho estourado: não adianta repetir aqui
        Stopped,    ///< `keep_going` pediu para parar; o `.part` fica para retomar
    };
    Outcome outcome = Outcome::Transient;
    long http_status = 0;
    std::string code;  ///< o `code` do `ota-failed`
    std::string message;
    std::int64_t total_bytes = 0;  ///< tamanho do `.part` no fim
    /// Só para Permanent: se o BACKEND deve tentar de novo (ex.: tamanho
    /// estourado pode ser um artefato republicado; 404 não).
    bool retryable = false;
};

class Downloader {
public:
    virtual ~Downloader() = default;
    virtual DownloadResult fetch(const DownloadRequest& request, const KeepGoing& keep_going) = 0;
};

/// O estado da unit que marca o slot bom NESTE boot (no RAUC,
/// `rauc-mark-good.service`, que roda `rauc status mark-good`).
enum class MarkGood {
    Pending,  ///< ainda não terminou (o hold-down leva minutos) ou nem rodou
    Good,     ///< saiu com 0: o slot foi marcado bom
    Failed,   ///< falhou; com a janela de teste aberta, a placa reinicia
};

class Platform {
public:
    virtual ~Platform() = default;
    /// `/proc/sys/kernel/random/boot_id`: muda a cada boot.
    virtual std::string boot_id() = 0;
    /// O slot em que este boot subiu (o `bootname` do RAUC: `"A"`,
    /// `"system0"`...); vazio fora de um slot (recovery, boot externo).
    virtual std::string current_slot() = 0;
    /// O slot que receberia a próxima instalação, partindo de `current`;
    /// vazio quando não se sabe.
    virtual std::string other_slot(const std::string& current) = 0;
    virtual MarkGood mark_good() = 0;
    /// O bootloader desistiu do slot (estado `bad`).
    virtual bool slot_bad(const std::string& slot) = 0;
    /// Pede o reboot. `true` = o pedido foi aceito (a placa vai cair).
    virtual bool reboot() = 0;
    /// Bytes livres para quem não é root na partição de `dir`; -1 = não sei.
    virtual std::int64_t free_bytes(const std::string& dir) = 0;
    /// Hora de parede (epoch ms): `applyAfter`, backoff persistido, `occurredAt`.
    virtual std::int64_t wall_ms() = 0;
    /// Monotônico (ms): prazos dentro do mesmo boot.
    virtual std::int64_t mono_ms() = 0;
    /// O token `dxd_` para o `device-auth`; vazio = não há. Nunca logar.
    virtual std::string device_token() = 0;
};

}  // namespace dextro::iot::ota
