#include <dextro-iot/procedures/envelope.hpp>

namespace dextro::iot::procedures {
namespace {

std::string text_of(const Json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::string_view major_of(std::string_view version) noexcept
{
    return version.substr(0, version.find('.'));
}

/// O `instance` vira pedaço de tópico. Um `+`, `#` ou `/` ali publicaria a
/// resposta num tópico curinga ou em outro ramo.
bool topic_safe(std::string_view level) noexcept
{
    if (level.empty() || level.size() > 128) {
        return false;
    }
    for (const char c : level) {
        if (c == '/' || c == '+' || c == '#' || c == '\0') {
            return false;
        }
    }
    return true;
}

/// `timestamp` em ms. A spec §2 aceita segundos: valores abaixo de 1e12 são
/// multiplicados por 1000. 0 = ausente ou ilegível.
std::int64_t timestamp_ms(const Json& metadata)
{
    const auto it = metadata.find("timestamp");
    if (it == metadata.end() || !it->is_number()) {
        return 0;
    }
    const auto value = it->is_number_integer() ? it->get<std::int64_t>()
                                               : static_cast<std::int64_t>(it->get<double>());
    if (value <= 0) {
        return 0;
    }
    return value < 1000000000000LL ? value * 1000 : value;
}

}  // namespace

Parsed parse_request(std::string_view text, std::string_view serial, const Clock& clock)
{
    Parsed out;
    Json root = Json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        out.problem = "envelope não é JSON";
        return out;
    }
    const auto meta = root.find("metadata");
    if (meta == root.end() || !meta->is_object()) {
        out.problem = "envelope sem metadata";
        return out;
    }

    Envelope& e = out.envelope;
    e.version = text_of(*meta, "v");
    e.message_id = text_of(*meta, "messageId");
    e.relation_id = text_of(*meta, "relationId");
    e.serial = text_of(*meta, "serialNumber");
    e.instance = text_of(*meta, "instance");
    e.op = text_of(root, "op");

    if (e.relation_id.empty() || !topic_safe(e.instance)) {
        out.problem = "envelope sem relationId ou instance válido";
        return out;
    }
    out.replyable = true;

    if (major_of(e.version) != major_of(kProtocolVersion)) {
        out.refusal = Reply::invalid("metadata.v " + e.version + " incompatível com " +
                                     std::string(kProtocolVersion));
        return out;
    }
    if (!e.serial.empty() && e.serial != serial) {
        out.refusal = Reply::error(Rescode::Forbidden, ErrorCode::InvalidArgs,
                                   "serialNumber " + e.serial + " não é este device");
        return out;
    }
    if (e.op.empty()) {
        out.refusal = Reply::invalid("envelope sem op");
        return out;
    }

    const std::int64_t sent_ms = timestamp_ms(*meta);
    if (clock.synchronized && sent_ms != 0) {
        const std::int64_t age_ms = clock.wall_ms - sent_ms;
        if (age_ms > kRequestTtlMs) {
            out.refusal = Reply::error(Rescode::Timeout, ErrorCode::TtlExpired,
                                       "pedido de " + std::to_string(age_ms / 1000) + " s atrás");
            return out;
        }
        if (-age_ms > kClockSkewMs) {
            out.refusal = Reply::error(Rescode::BadRequest, ErrorCode::ClockSkew,
                                       "pedido " + std::to_string(-age_ms / 1000) + " s no futuro");
            return out;
        }
    }

    if (const auto payload = root.find("payload"); payload != root.end() && !payload->is_null()) {
        e.payload = std::move(*payload);
    }
    return out;
}

std::string build_reply(const Envelope& request, const Reply& reply, std::string_view serial,
                        std::string_view message_id, std::int64_t wall_ms)
{
    Json metadata = {
        {"v", std::string(kProtocolVersion)},
        {"messageId", std::string(message_id)},
        {"relationId", request.relation_id},
        {"timestamp", wall_ms},
        {"serialNumber", std::string(serial)},
        {"instance", request.instance},
    };
    Json out = {
        {"metadata", std::move(metadata)},
        {"op", request.op},
        {"payload", reply.payload},
        {"rescode", value(reply.rescode)},
    };
    // `error_handler_t::replace`: um byte inválido em UTF-8 vindo de algum
    // campo livre não pode virar exceção aqui — viraria resposta nenhuma.
    return out.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string reply_topic(std::string_view serial, std::string_view instance)
{
    std::string topic = "atrio/";
    topic += serial;
    topic += "/procedure/reply/";
    topic += instance;
    return topic;
}

}  // namespace dextro::iot::procedures
