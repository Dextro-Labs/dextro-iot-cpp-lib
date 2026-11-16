#include <dextro-iot/procedures/procedure.hpp>

#include <algorithm>
#include <cctype>
#include <exception>

namespace dextro::iot::procedures {

Reply Reply::ok(Json payload)
{
    Reply reply;
    reply.rescode = Rescode::Ok;
    reply.payload = std::move(payload);
    return reply;
}

Reply Reply::accepted(Json payload)
{
    Reply reply;
    reply.rescode = Rescode::Accepted;
    reply.payload = std::move(payload);
    return reply;
}

Reply Reply::no_content()
{
    Reply reply;
    reply.rescode = Rescode::NoContent;
    return reply;
}

Reply Reply::error(Rescode rescode, ErrorCode code, std::string detail)
{
    Reply reply;
    reply.rescode = rescode;
    reply.payload = Json{{"error", std::string(error_name(code))}};
    if (!detail.empty()) {
        reply.payload["detail"] = std::move(detail);
    }
    return reply;
}

Reply Reply::error(Rescode rescode, std::string_view code, std::string detail)
{
    Reply reply;
    reply.rescode = rescode;
    reply.payload = Json{{"error", std::string(code)}};
    if (!detail.empty()) {
        reply.payload["detail"] = std::move(detail);
    }
    return reply;
}

Reply Reply::invalid(std::string detail)
{
    return error(Rescode::BadRequest, ErrorCode::InvalidArgs, std::move(detail));
}

Reply Procedure::handle(const Request& request) noexcept
{
    try {
        if (!request.payload.is_object()) {
            return Reply::invalid("payload não é objeto");
        }
        if (auto refused = validate(request.payload)) {
            return std::move(*refused);
        }
        return execute(request);
    } catch (const std::exception& e) {
        // O nlohmann lança em acesso de tipo errado; um port pode lançar
        // bad_alloc. Nenhum dos dois pode chegar ao laço da thread.
        try {
            return Reply::error(Rescode::ServerError, ErrorCode::Internal, e.what());
        } catch (...) {
        }
    } catch (...) {
    }
    // Sem memória nem para montar a resposta de erro: o rescode sozinho ainda
    // diz ao backend o que aconteceu.
    Reply reply;
    reply.rescode = Rescode::ServerError;
    return reply;
}

// --- validação --------------------------------------------------------------

std::optional<Reply> only_keys(const Json& payload, std::initializer_list<std::string_view> allowed)
{
    for (const auto& item : payload.items()) {
        if (std::find(allowed.begin(), allowed.end(), item.key()) == allowed.end()) {
            return Reply::invalid("campo não previsto: " + item.key());
        }
    }
    return std::nullopt;
}

std::optional<Reply> require(const Json& payload, std::string_view key)
{
    if (!payload.contains(key)) {
        return Reply::invalid("falta o campo " + std::string(key));
    }
    return std::nullopt;
}

std::optional<Reply> string_field(const Json& payload, std::string_view key, std::size_t max_len)
{
    const auto it = payload.find(key);
    if (it == payload.end()) {
        return std::nullopt;
    }
    if (!it->is_string()) {
        return Reply::invalid(std::string(key) + " não é texto");
    }
    if (max_len != 0 && it->get_ref<const std::string&>().size() > max_len) {
        return Reply::invalid(std::string(key) + " passa de " + std::to_string(max_len) + " bytes");
    }
    return std::nullopt;
}

std::optional<Reply> integer_field(const Json& payload, std::string_view key, std::int64_t min)
{
    const auto it = payload.find(key);
    if (it == payload.end()) {
        return std::nullopt;
    }
    if (!it->is_number_integer() || it->get<std::int64_t>() < min) {
        return Reply::invalid(std::string(key) + " não é inteiro >= " + std::to_string(min));
    }
    return std::nullopt;
}

bool is_uuid(std::string_view text) noexcept
{
    // 8-4-4-4-12, hex. A versão não é conferida: o backend gera v4 e v7.
    if (text.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? text[i] != '-' : std::isxdigit(static_cast<unsigned char>(text[i])) == 0) {
            return false;
        }
    }
    return true;
}

bool is_sha256_hex(std::string_view text) noexcept
{
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
               return std::isxdigit(static_cast<unsigned char>(c)) != 0;
           });
}

bool is_version_token(std::string_view text) noexcept
{
    if (text.empty() || text.size() > 64) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '+' || c == '-';
    });
}

bool is_semver(std::string_view text) noexcept
{
    // MAJOR.MINOR.PATCH, com pré-release/build opcionais depois de `-`/`+`.
    // O núcleo numérico é o que importa para comparar versão instalada.
    int dots = 0;
    bool digit = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '-' || c == '+') {
            return dots == 2 && digit && i + 1 < text.size();
        }
        if (c == '.') {
            if (!digit) {
                return false;
            }
            ++dots;
            digit = false;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
        digit = true;
    }
    return dots == 2 && digit;
}

}  // namespace dextro::iot::procedures
