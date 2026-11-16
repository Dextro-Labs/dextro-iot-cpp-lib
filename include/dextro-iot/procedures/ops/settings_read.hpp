#pragma once

#include <dextro-iot/procedures/procedure_table.hpp>

/// `settings-read` (contracts.md A.4.5): a coleção **efetivamente aplicada**
/// no device, filtrada por `refs` quando vier.
///
/// Paginação opcional: `from` (posição na coleção, a partir de 0) e `count`
/// (1..kMaxPage). Com qualquer um dos dois, a resposta leva só a página e
/// `more` (há itens depois dela); sem nenhum, a coleção inteira e sem `more`,
/// exatamente como antes. A posição é sobre a coleção já filtrada por `refs`,
/// na ordem em que o device a devolve (a mesma em toda leitura).
namespace dextro::iot::procedures {

class SettingsRead final : public Procedure {
public:
    explicit SettingsRead(const SettingsStore* settings);

    std::optional<Reply> validate(const Json& payload) const override;
    Reply execute(const Request& request) override;

    /// Maior página, igual ao teto de itens do `settings-write`.
    static constexpr std::size_t kMaxPage = 500;

private:
    const SettingsStore* settings_;
};

}  // namespace dextro::iot::procedures
