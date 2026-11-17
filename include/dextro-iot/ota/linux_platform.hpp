#pragma once

#include <dextro-iot/device_token.hpp>
#include <dextro-iot/ota/ports.hpp>

#include <string>
#include <vector>

/// A `Platform` de um Linux com systemd e RAUC.
///
/// ## O que é parâmetro (era fixo no SSAT)
///
///   - **o slot corrente**: a chave da cmdline (`rauc.slot=`, a forma
///     genérica do manual do RAUC; `ssat.slot=` no controlador SSAT). Sem a
///     chave na cmdline (ou com a chave vazia na configuração), vale o
///     `RAUC_SYSTEM_BOOTED_BOOTNAME` do `rauc status`;
///   - **o slot alvo e o "slot bad"**: do `rauc status --output-format=shell`
///     (`RAUC_SLOT_BOOTNAME_n`, `RAUC_SLOT_BOOT_STATUS_n`) — o RAUC pergunta
///     ao backend do bootloader, inclusive a um backend `custom` (no SSAT era o
///     `ssat-boot-switch get-state`, chamado direto);
///   - **a unit do mark-good** (`rauc-mark-good.service` por padrão), lida por
///     `systemctl show`: saiu com 0 NESTE boot = bom;
///   - **o token** (`DeviceTokenSource`).
///
/// ## Privilégio
///
/// `rauc install`/`rauc status` são clientes D-Bus do `rauc.service`;
/// `systemctl reboot`/`show` vão pelo D-Bus ao PID 1. O processo não precisa
/// abrir bloco nenhum — só ler o token.
///
/// Origem: `SystemPlatform` do ssat-ota-agent (`src/ota/system.cpp`,
/// `origin/devel` 76aa940).
namespace dextro::iot::ota {

/// Roda `argv` e espera, chamando `tick` a cada segundo (watchdog). `output`
/// recebe o fim do stdout+stderr (até 4 KiB). Devolve o código de saída, ou
/// -1 se nem rodou.
///
/// O filho NÃO herda `NOTIFY_SOCKET`/`WATCHDOG_*`/`LISTEN_*`: o `rauc` e o
/// `systemctl` que falam sd_notify cairiam no socket deste processo — e um
/// filho mandando `WATCHDOG=1` alimentaria o watchdog no lugar dele.
int run_command(const std::vector<std::string>& argv, std::string& output,
                const KeepGoing& tick = {});

class LinuxPlatform final : public Platform {
public:
    class Config {
    public:
        /// A chave do slot na cmdline, com o `=`. Vazio = só o `rauc status`.
        std::string slot_cmdline_key = "rauc.slot=";
        /// O valor da cmdline em maiúsculas (`ssat.slot=a` → `"A"`, como o
        /// SSAT reportava).
        bool slot_upper_case = false;
        std::string mark_good_unit = "rauc-mark-good.service";
        std::string rauc = "rauc";
        std::string systemctl = "systemctl";
        DeviceTokenSource token;
    };

    explicit LinuxPlatform(Config config);

    std::string boot_id() override;
    std::string current_slot() override;
    std::string other_slot(const std::string& current) override;
    MarkGood mark_good() override;
    bool slot_bad(const std::string& slot) override;
    bool reboot() override;
    std::int64_t free_bytes(const std::string& dir) override;
    std::int64_t wall_ms() override;
    std::int64_t mono_ms() override;
    std::string device_token() override;

    const Config& config() const noexcept { return config_; }

private:
    Config config_;
};

/// O `systemctl show -p ActiveState -p Result -p ExecMainStatus
/// -p ExecMainExitTimestampMonotonic <unit>`, lido.
MarkGood parse_mark_good(const std::string& show_output);

/// O valor de `key` (`"rauc.slot="`) na cmdline; vazio se não houver.
std::string slot_from_cmdline(const std::string& cmdline, const std::string& key);

}  // namespace dextro::iot::ota
