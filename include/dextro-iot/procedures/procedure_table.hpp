#pragma once

#include <dextro-iot/procedures/ports.hpp>
#include <dextro-iot/procedures/procedure.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// O registro das device-procedures: uma linha por op, e cada linha com a
/// classe que a atende.
///
/// No ssat-traffic-controller isto era um array `constexpr` na ordem de um
/// enum `ProcedureId` com as ops do controlador de trânsito. Aqui a biblioteca
/// não conhece as ops da aplicação: a aplicação **registra** cada uma pelo
/// nome canônico do fio (`add()`), e a biblioteca só garante o resto — nome
/// único, despacho por nome, porteiro de escopo e origem, e o `list-procedures`
/// saindo da tabela, e não de uma lista escrita à mão.
///
/// A spec chama isto de "tabela estática de handlers" para o binding C++
/// (dextro-iot-spec §14).
namespace dextro::iot::procedures {

/// Por onde o pedido chegou. A op é uma só — a mesma classe, o mesmo
/// `handle()` —, e o que muda é quem pode chamá-la.
enum class Origin {
    /// Device-procedure direta do backend (`atrio/{sn}/procedure`).
    Rpc,
    /// Item da outbox durável (contracts.md A.3.4), drenado pela aplicação.
    Outbox,
    /// Um canal local da aplicação (no controlador SSAT, o tablet pelo
    /// Bluetooth). Admite as `outbox_only`: a outbox existe para quem escreve
    /// sem estar lá; o canal local escreve com resposta síncrona.
    Local,
};

/// Uma linha da tabela.
class ProcedureSpec {
public:
    /// O nome canônico da op no fio (`"get-status"`).
    std::string op;
    /// A versão do contrato da op que este firmware implementa. Vai no
    /// `list-procedures` e governa a escolha de schema no backend (A.4.1).
    int version = 1;
    /// Atendida também quando a sessão está com o certificado de fábrica.
    bool bootstrap = false;
    /// O timeout default do contrato (A.4). Informativo: quem mede é o backend.
    int timeout_ms = 30000;
    /// Só pela outbox durável: pela RPC direta responde 403 `OUTBOX_ONLY`. É a
    /// classe das escritas de configuração, que não podem se perder com o
    /// device offline nem chegar por um caminho sem reentrega.
    bool outbox_only = false;
};

/// O porteiro da linha, antes do `handle()`: escopo bootstrap e origem.
/// `nullopt` = passa.
///
///   - sessão bootstrap e op não-bootstrap → 403 `OUT_OF_SCOPE`
///   - op `outbox_only` pedida pela RPC    → 403 `OUTBOX_ONLY` (pela outbox
///     ou por um canal local, passa)
std::optional<Reply> admit(const ProcedureSpec& row, bool bootstrap_scope, Origin origin);

class ProcedureTable {
public:
    ProcedureTable() = default;
    ProcedureTable(const ProcedureTable&) = delete;
    ProcedureTable& operator=(const ProcedureTable&) = delete;

    /// Registra uma op. `false` (e nada registrado) com nome vazio, nome já
    /// registrado ou classe nula. A classe fica com a tabela; os ports que ela
    /// guarda têm que viver mais que a tabela.
    bool add(ProcedureSpec spec, std::unique_ptr<Procedure> procedure);

    /// Despacha por nome de op. Não lança.
    ///
    ///   - op fora da tabela                  → 404 `UNKNOWN_OP`
    ///   - o que o `admit()` recusar           → 403
    ///   - senão, o `handle()` da classe
    Reply dispatch(std::string_view op, const Request& request, bool bootstrap_scope,
                   Origin origin = Origin::Rpc) noexcept;

    /// O mesmo, a partir do payload em texto:
    ///
    ///   - maior que `kMaxPayload`       → 413 (spec §9)
    ///   - JSON ilegível                 → 400 `INVALID_ARGS`
    ///   - `idempotencyKey` sai do payload e vai para o `Request` — é campo
    ///     do protocolo (spec §5), não da op, e o `additionalProperties: false`
    ///     de cada op o recusaria.
    Reply dispatch_text(std::string_view op, std::string_view payload, std::string relation_id,
                        bool bootstrap_scope, Origin origin = Origin::Rpc) noexcept;

    /// O mesmo, com o payload já decodificado (vindo do envelope). Tira a
    /// `idempotencyKey` como o `dispatch_text`.
    Reply dispatch_payload(std::string_view op, Json payload, std::string relation_id,
                           bool bootstrap_scope, Origin origin = Origin::Rpc) noexcept;

    /// `MAX_PAYLOAD` da spec §9 (`ATRIO_MAX_PAYLOAD_BYTES`, SPEC §4.2).
    static constexpr std::size_t kMaxPayload = 256 * 1024;

    /// A classe de uma op, ou nulo.
    Procedure* at(std::string_view op) const noexcept;
    /// A linha de uma op, ou nulo.
    const ProcedureSpec* spec(std::string_view op) const noexcept;

    std::size_t size() const noexcept { return rows_.size(); }

    /// Visita as linhas na ordem de registro.
    template <typename Fn>
    void for_each(Fn&& fn) const
    {
        for (const Row& row : rows_) {
            fn(row.spec);
        }
    }

private:
    class Row {
    public:
        ProcedureSpec spec;
        std::unique_ptr<Procedure> procedure;
    };
    const Row* find(std::string_view op) const noexcept;

    std::vector<Row> rows_;
};

}  // namespace dextro::iot::procedures
