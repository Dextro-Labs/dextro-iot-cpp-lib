#pragma once

#include <dextro-iot/procedures/procedure.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

/// O envelope do Dextro IoT (spec §2–§3): `{metadata, op, payload}` na ida e
/// `{metadata, op, payload, rescode}` na volta.
///
/// Puro: o id da mensagem e o relógio de parede vêm de quem chama.
///
/// ## O que é conferido, e o que acontece
///
///   - JSON ilegível, ou sem `metadata.relationId`/`instance`: **não há a quem
///     responder** — a resposta vai para `.../reply/{instance}` e ecoa o
///     `relationId`. Descarta com log;
///   - `metadata.v` com major diferente do nosso → 400 (spec §2);
///   - `metadata.serialNumber` diferente do nosso → 403 (spec §9: o device não
///     confia no broker para isso);
///   - `op` ausente → 400; desconhecida → 404, pela tabela;
///   - `metadata.timestamp` mais velho que `kRequestTtlMs` → 504
///     `TTL_EXPIRED` (spec §8: nunca descarte silencioso); mais de
///     `kClockSkewMs` no futuro → 400 `CLOCK_SKEW` (spec §2).
///
/// ## O TTL só vale com relógio sincronizado
///
/// A spec §8 manda: relógio não sincronizado ⇒ TTL desligado com WARN, nunca
/// descartar. Um controlador que acabou de ligar, antes do GNSS/NTP, está em
/// 1970 ou na hora do RTC — com TTL ligado, ele recusaria todo pedido como
/// "do futuro". Quem diz se está sincronizado é o `Clock`, que o driver
/// preenche com o estado do kernel (`adjtimex`, o mesmo que o chrony mantém).
namespace dextro::iot::procedures {

/// `metadata.v` que este device fala.
constexpr std::string_view kProtocolVersion = "0.5";

/// TTL de request RPC no device: 120 s no inbound (dextrolabs-device SPEC.md
/// §4.2, "request expirado (TTL de 120s no inbound) → 504 TTL_EXPIRED"). O
/// ssat-trafficd usa 60 s; a lib segue a spec.
constexpr std::int64_t kRequestTtlMs = 120000;
/// `CLOCK_SKEW_TOL` (spec §2).
constexpr std::int64_t kClockSkewMs = 5000;

/// O relógio de parede do device, e se dá para confiar nele.
class Clock {
public:
    std::int64_t wall_ms = 0;
    bool synchronized = false;
};

class Envelope {
public:
    std::string version;
    std::string message_id;
    std::string relation_id;
    std::string serial;
    std::string instance;
    std::string op;
    Json payload = Json::object();
};

class Parsed {
public:
    Envelope envelope;
    /// `false` = sem `relationId`/`instance`: não há como responder.
    bool replyable = false;
    /// Preenchido quando o envelope é recusado antes de chegar à tabela.
    std::optional<Reply> refusal;
    /// Por que não dá para responder, para o log.
    std::string problem;
};

/// Decodifica um request de device-procedure. `serial` é o nosso. Sem
/// `clock` (ou com ele não sincronizado), o TTL não é aplicado.
Parsed parse_request(std::string_view text, std::string_view serial, const Clock& clock = Clock{});

/// Monta a resposta. `message_id` é um uuid novo; `wall_ms`, o relógio de parede.
std::string build_reply(const Envelope& request, const Reply& reply, std::string_view serial,
                        std::string_view message_id, std::int64_t wall_ms);

/// O tópico da resposta: `atrio/{sn}/procedure/reply/{instance}`.
std::string reply_topic(std::string_view serial, std::string_view instance);

}  // namespace dextro::iot::procedures
