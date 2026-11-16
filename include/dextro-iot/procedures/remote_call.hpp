#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

/// As remote-procedures: o device chama, o backend atende (SPEC do
/// dextrolabs-device §4.1–4.3). O pedido sai em `atrio/{sn}/service` e a
/// resposta volta em `atrio/{sn}/service/reply`, casada pelo `relationId`.
///
/// Puro: não publica, não lê relógio e não loga. Quem tem a sessão (o
/// `iot::Client`) publica o texto que `begin()` devolve, entrega o que chega
/// em `service/reply` a `on_reply()` e chama `expire()` a cada volta. É o que
/// deixa o casamento, o prazo e a queda de sessão testáveis sem broker.
///
/// ## As três saídas de um pedido, e só três
///
///   - a resposta chegou: o `done` recebe o `rescode` e o `payload` dela;
///   - o prazo venceu: `done` com 504;
///   - a sessão caiu antes: `done` com 503 (`fail_all`).
///
/// Todo pedido termina por exatamente uma delas — quem pede nunca fica
/// esperando para sempre.
namespace dextro::iot::procedures {

class RemoteReply {
public:
    int rescode = 0;
    nlohmann::json payload = nlohmann::json::object();
    /// O `error` do payload numa resposta de erro, ou o motivo local (prazo,
    /// sessão caída, buffer cheio).
    std::string error;

    bool ok() const noexcept { return rescode >= 200 && rescode < 300; }
};

class RemoteCalls {
public:
    using Done = std::function<void(const RemoteReply&)>;

    static constexpr std::int64_t kDefaultTimeoutMs = 30000;  ///< spec: 30 s
    /// Teto de pedidos em voo. O backend aceita 20 req/s por SN; mais que isto
    /// em voo é defeito de quem chama, e o pedido volta 429 na hora.
    static constexpr std::size_t kMaxPending = 64;

    class Request {
    public:
        std::string op;
        nlohmann::json payload = nlohmann::json::object();
        std::string serial;
        std::string message_id;
        std::string relation_id;
        std::int64_t wall_ms = 0;
        std::int64_t now_ms = 0;
        std::int64_t timeout_ms = kDefaultTimeoutMs;
    };

    /// Registra o pedido e devolve o envelope a publicar. Vazio = recusado na
    /// hora, e o `done` já foi chamado (429 com a fila cheia).
    std::string begin(const Request& request, Done done);

    /// Desiste de um pedido que não chegou a sair (publish falhou): `done` com
    /// o `rescode` dado.
    void cancel(const std::string& relation_id, int rescode, std::string_view why);

    /// Uma mensagem de `service/reply`. `true` se casou com um pendente.
    bool on_reply(std::string_view text);

    /// Vence os pendentes cujo prazo passou (504).
    void expire(std::int64_t now_ms);

    /// A sessão caiu: todos os pendentes terminam com `rescode`.
    void fail_all(int rescode, std::string_view why);

    std::size_t pending() const noexcept { return pending_.size(); }

    /// Em quantos ms vence o próximo pendente (-1 = nenhum).
    std::int64_t next_deadline_in(std::int64_t now_ms) const noexcept;

private:
    class Pending {
    public:
        std::string op;
        std::int64_t deadline_ms = 0;
        Done done;
    };
    std::map<std::string, Pending> pending_;
};

}  // namespace dextro::iot::procedures
