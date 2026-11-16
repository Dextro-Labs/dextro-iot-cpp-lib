#pragma once

#include <dextro-iot/procedures/procedure_table.hpp>

/// `settings-write` (contracts.md A.4.6).
///
/// Vem sempre a coleção completa. Item que o device não pode aplicar — chave
/// que não existe, valor fora do tipo, identidade de fábrica — vai para
/// `rejected[]` com o motivo, e o resto é aplicado: recusar a coleção inteira
/// por um item deixaria o device sem nenhuma das mudanças boas.
///
/// `version` menor que a aplicada é 409 `STALE_VERSION`: é reentrega fora de
/// ordem pela outbox, e aplicá-la desfaria uma edição mais nova.
namespace dextro::iot::procedures {

class SettingsWrite final : public Procedure {
public:
    /// `SettingsCollection.maxItems`.
    static constexpr std::size_t kMaxItems = 500;

    /// `session` pode ser nulo: sem ele, um broker trocado só vale na próxima
    /// reabertura que a aplicação fizer.
    SettingsWrite(SettingsStore* settings, SessionControl* session);

    std::optional<Reply> validate(const Json& payload) const override;
    Reply execute(const Request& request) override;

private:
    SettingsStore* settings_;
    SessionControl* session_;
};

}  // namespace dextro::iot::procedures
