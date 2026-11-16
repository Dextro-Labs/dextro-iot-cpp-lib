#include <dextro-iot/procedures/procedure_table.hpp>

namespace dextro::iot::procedures {

std::optional<Reply> admit(const ProcedureSpec& row, bool bootstrap_scope, Origin origin)
{
    if (bootstrap_scope && !row.bootstrap) {
        // O ACL do EMQX já não deixa o backend publicar isto no escopo
        // bootstrap (SPEC do dextrolabs-device §5). Recusar aqui também é
        // a segunda camada: o device não confia no broker para isso.
        return Reply::error(Rescode::Forbidden, ErrorCode::OutOfScope, row.op);
    }
    if (row.outbox_only && origin == Origin::Rpc) {
        // Escrita de configuração só pela outbox: entrega durável, com
        // reentrega e ack. Pela RPC direta ela se perderia com o device
        // offline, e o resultado não voltaria pela inbox.
        return Reply::error(Rescode::Forbidden, ErrorCode::OutboxOnly, row.op);
    }
    return std::nullopt;
}

bool ProcedureTable::add(ProcedureSpec spec, std::unique_ptr<Procedure> procedure)
{
    if (spec.op.empty() || !procedure || find(spec.op) != nullptr) {
        return false;
    }
    Row row;
    row.spec = std::move(spec);
    row.procedure = std::move(procedure);
    rows_.push_back(std::move(row));
    return true;
}

const ProcedureTable::Row* ProcedureTable::find(std::string_view op) const noexcept
{
    for (const Row& row : rows_) {
        if (row.spec.op == op) {
            return &row;
        }
    }
    return nullptr;
}

Procedure* ProcedureTable::at(std::string_view op) const noexcept
{
    const Row* row = find(op);
    return row != nullptr ? row->procedure.get() : nullptr;
}

const ProcedureSpec* ProcedureTable::spec(std::string_view op) const noexcept
{
    const Row* row = find(op);
    return row != nullptr ? &row->spec : nullptr;
}

Reply ProcedureTable::dispatch(std::string_view op, const Request& request,
                               bool bootstrap_scope, Origin origin) noexcept
{
    try {
        const Row* row = find(op);
        if (row == nullptr) {
            return Reply::error(Rescode::NotFound, ErrorCode::UnknownOp, std::string(op));
        }
        if (auto refused = admit(row->spec, bootstrap_scope, origin)) {
            return std::move(*refused);
        }
        return row->procedure->handle(request);
    } catch (...) {
        Reply reply;
        reply.rescode = Rescode::ServerError;
        return reply;
    }
}

Reply ProcedureTable::dispatch_text(std::string_view op, std::string_view payload,
                                    std::string relation_id, bool bootstrap_scope,
                                    Origin origin) noexcept
{
    try {
        if (payload.size() > kMaxPayload) {
            return Reply::error(Rescode::PayloadTooLarge, ErrorCode::InvalidArgs,
                                std::to_string(payload.size()) + " bytes");
        }
        // Payload vazio é o mesmo que `{}`: `list-procedures` não tem campo.
        Json parsed = payload.empty() ? Json::object()
                                      : Json::parse(payload, nullptr, /*allow_exceptions=*/false);
        if (parsed.is_discarded()) {
            return Reply::invalid("payload não é JSON");
        }
        return dispatch_payload(op, std::move(parsed), std::move(relation_id), bootstrap_scope,
                                origin);
    } catch (...) {
        Reply reply;
        reply.rescode = Rescode::ServerError;
        return reply;
    }
}

Reply ProcedureTable::dispatch_payload(std::string_view op, Json payload, std::string relation_id,
                                       bool bootstrap_scope, Origin origin) noexcept
{
    try {
        Request request;
        request.relation_id = std::move(relation_id);
        request.payload = std::move(payload);
        if (request.payload.is_object()) {
            if (const auto it = request.payload.find("idempotencyKey");
                it != request.payload.end()) {
                if (!it->is_string()) {
                    return Reply::invalid("idempotencyKey não é texto");
                }
                request.idempotency_key = it->get<std::string>();
                request.payload.erase(it);
            }
        }
        return dispatch(op, request, bootstrap_scope, origin);
    } catch (...) {
        Reply reply;
        reply.rescode = Rescode::ServerError;
        return reply;
    }
}

}  // namespace dextro::iot::procedures
