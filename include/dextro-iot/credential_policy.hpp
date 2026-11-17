#pragma once

#include <cstdint>
#include <optional>

/// Com qual certificado a sessão tenta: o operacional ou o de fábrica.
///
/// ## A regra: nunca um estado sem saída
///
/// O broker recusar o certificado operacional três vezes seguidas quer dizer,
/// no caso comum, que ele expirou ou foi revogado — e aí o certo é voltar ao de
/// fábrica, onde o service refaz o fluxo inicial (certificate-request →
/// certificate-install) e devolve um operacional novo.
///
/// Só que a recusa também chega por motivos que passam: o hook de
/// autenticação do EMQX fora do ar responde CONNACK 5 para todo mundo. E o de
/// fábrica também pode estar vencido. Uma política que fosse para o bootstrap e
/// **não voltasse mais** deixaria, com meia hora de hook fora, a frota inteira
/// presa no escopo de fábrica, sem telemetria nem procedures, até alguém ir ao
/// device (aconteceu no controlador SSAT).
///
/// Então a política alterna:
///
///   - no operacional: 3 recusas seguidas → tenta o de fábrica (se existir);
///   - no de fábrica: 3 recusas seguidas, **ou** 10 min sem conectar → tenta o
///     operacional de novo (se existir).
///
/// Com o backoff da sessão por baixo, isso vira um revezamento lento e sem fim,
/// que para no primeiro certificado que o broker aceitar. Sem um dos dois, fica
/// no que existe — e continua tentando.
///
/// Pura: sem relógio, sem disco e sem log. Quem chama passa o tempo
/// (`clock::monotonic_ms()`) e o que existe em disco; quem troca de fato é a
/// aplicação, chamando `Client::start()` com o outro par e depois
/// `switched()`.
///
/// ```cpp
/// dextro::iot::CredentialPolicy policy;
/// // a cada volta do laço:
/// dextro::iot::CredentialPolicy::Inputs in;
/// in.current = using_factory ? Scope::Bootstrap : Scope::Operational;
/// in.unauthorized_streak = client.unauthorized_streak();
/// in.connected = client.connected();
/// in.has_operational = exists(op_cert); in.has_bootstrap = exists(factory_cert);
/// in.now_ms = now;
/// if (auto to = policy.decide(in)) {
///     client.start(config_for(*to), now);
///     policy.switched(client.unauthorized_streak(), now);
/// }
/// ```
///
/// Origem: `src/services/iot-manager/credential_policy.{hpp,cpp}` do
/// ssat-traffic-controller (`origin/devel` 67a0f77), sem mudança de regra.
namespace dextro::iot {

class CredentialPolicy {
public:
    enum class Scope { Operational, Bootstrap };

    static constexpr int kRefusalsBeforeSwitch = 3;
    static constexpr std::int64_t kBootstrapMaxMs = 10LL * 60 * 1000;

    class Inputs {
    public:
        Scope current = Scope::Operational;
        /// `Client::unauthorized_streak()` (não zera ao trocar de
        /// certificado; a política guarda a base).
        int unauthorized_streak = 0;
        bool connected = false;
        bool has_operational = false;
        bool has_bootstrap = false;
        std::int64_t now_ms = 0;
    };

    /// Para qual escopo trocar agora, ou nada.
    std::optional<Scope> decide(const Inputs& in);

    /// A troca aconteceu (ou a sessão foi reaberta de outro jeito): as contas
    /// recomeçam deste ponto.
    void switched(int unauthorized_streak, std::int64_t now_ms) noexcept;

private:
    int streak_base_ = 0;
    std::int64_t scope_since_ms_ = 0;
    std::int64_t last_connected_ms_ = 0;
    bool started_ = false;
};

}  // namespace dextro::iot
