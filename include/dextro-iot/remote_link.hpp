#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

/// O que os componentes de cima da sessão (`Keepalive`, `EventDispatcher`)
/// precisam do `Client`: chamar uma remote-procedure, saber se há sessão e
/// pedir a reabertura.
///
/// Interface para esses componentes rodarem no CI contra um backend falso —
/// sem broker, sem TLS —, que é onde as respostas ruins (503, 504, 413) são
/// testadas de verdade. O `Client` é a implementação de produção.
namespace dextro::iot {

/// A resposta de uma remote-procedure, sem JSON no header: o payload vai como
/// texto e quem chama decodifica.
class RemoteResult {
public:
    int rescode = 0;
    std::string payload_json = "{}";
    std::string error;

    bool ok() const noexcept { return rescode >= 200 && rescode < 300; }
};

class RemoteLink {
public:
    using RemoteDone = std::function<void(const RemoteResult&)>;

    virtual ~RemoteLink() = default;

    /// `true` com a sessão MQTT aberta (CONNACK 0 recebido).
    virtual bool connected() const noexcept = 0;

    /// Chama uma remote-procedure do backend. O `done` roda sempre, uma vez,
    /// na thread do `tick()`: com o `rescode` do backend, ou 503 sem sessão,
    /// 504 no prazo, 413 grande demais, 429 fila cheia, 400 payload que não é
    /// objeto JSON. `timeout_ms <= 0` = o padrão (30 s).
    virtual void call(std::string_view op, std::string_view payload_json, std::int64_t now_ms,
                      std::int64_t timeout_ms, RemoteDone done) = 0;

    /// Fecha a sessão e reabre agora, sem backoff e sem contar como queda.
    virtual void restart(std::int64_t now_ms) = 0;
};

}  // namespace dextro::iot
