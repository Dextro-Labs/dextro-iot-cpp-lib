#pragma once

/// "A rede mudou", do kernel, sem sondar.
///
/// Um socket netlink assinando interface, endereço e rota (IPv4 e IPv6). É o
/// que transforma "o 4G voltou" em reconexão **na hora**, em vez de esperar o
/// fim do backoff — e "o 4G trocou de IP" em sonda imediata da sessão aberta,
/// em vez de descobrir no próximo keepalive que o caminho antigo morreu.
///
/// Só avisa; não interpreta. Qualquer mudança conta: interface subindo,
/// endereço novo, rota default trocada (failover 4G ↔ Wi-Fi, VPN subindo).
/// Falso positivo custa um PING; falso negativo custaria a sessão.
namespace dextro::iot::session {

class LinkMonitor {
public:
    LinkMonitor() = default;
    ~LinkMonitor();

    LinkMonitor(const LinkMonitor&) = delete;
    LinkMonitor& operator=(const LinkMonitor&) = delete;

    /// `false` quando o netlink não abre (contêiner sem rede, seccomp). A
    /// sessão segue sem ele: perde a reconexão imediata, não a reconexão.
    bool open() noexcept;
    void close() noexcept;

    int fd() const noexcept { return fd_; }

    /// Esvazia o que chegou. `true` se havia ao menos um evento de rede.
    bool drain() noexcept;

private:
    int fd_ = -1;
};

}  // namespace dextro::iot::session
