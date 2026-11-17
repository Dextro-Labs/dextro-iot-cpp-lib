#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string_view>

/// Para onde vai a trilha do OTA: os eventos `ota-*` do catálogo do tipo de
/// device (no dextrolabs-device, migrations 0050/0082 e, para a estação
/// AllPrint, 0107: numeric_id 40..49).
///
/// No ssat-ota-agent isto era uma pasta de arquivos (`Outbox`) que o
/// `ssat-trafficd` lia e passava à fila dele. Aqui é uma porta: a aplicação
/// grava o evento na fila persistente dela (o `EventStore` do
/// `EventDispatcher`) e chama `EventDispatcher::kick()`. A biblioteca não
/// embute fila nem banco.
///
/// Header só de interface: vale com ou sem o módulo `ota` compilado (a op
/// `ota-notification` e o `InboxOtaSpool` também o usam, para o
/// `ota-received`).
///
/// **De que thread.** O `ota::Engine` chama `emit()` da thread em que ele roda
/// — que NÃO é a do `Client` (o `Engine` bloqueia; ver `ota/engine.hpp`). Se a
/// fila da aplicação também é tocada pelo laço do `Client`, a implementação
/// tem de ser segura para as duas threads (um mutex em volta da gravação, e o
/// `kick()` levado ao laço do `Client` por um eventfd/pipe, nunca chamado
/// direto de outra thread).
namespace dextro::iot::ota {

class EventSink {
public:
    virtual ~EventSink() = default;

    /// Grava um evento. `code` é o `code` do catálogo (`"ota-verified"`),
    /// `args` o objeto de argumentos (sempre com `campaignItemId` e `version`),
    /// `occurred_ms` a hora de parede (epoch ms) do fato.
    ///
    /// `false` = não gravou. A atualização NÃO para por isso (a trilha é
    /// informativa, o estado do item está no disco): quem chama loga e segue.
    virtual bool emit(std::string_view code, const nlohmann::json& args,
                      std::int64_t occurred_ms) = 0;
};

}  // namespace dextro::iot::ota
