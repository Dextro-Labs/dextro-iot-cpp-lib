#include <dextro-iot/procedures/rescode.hpp>

namespace dextro::iot::procedures {

std::string_view error_name(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::InvalidArgs:
        return "INVALID_ARGS";
    case ErrorCode::UnknownOp:
        return "UNKNOWN_OP";
    case ErrorCode::OutOfScope:
        return "OUT_OF_SCOPE";
    case ErrorCode::OutboxOnly:
        return "OUTBOX_ONLY";
    case ErrorCode::Internal:
        return "INTERNAL";
    case ErrorCode::Unavailable:
        return "UNAVAILABLE";
    case ErrorCode::TtlExpired:
        return "TTL_EXPIRED";
    case ErrorCode::ClockSkew:
        return "CLOCK_SKEW";
    case ErrorCode::SignatureInvalid:
        return "SIGNATURE_INVALID";
    case ErrorCode::ChecksumInvalid:
        return "CHECKSUM_INVALID";
    case ErrorCode::VersionNotAllowed:
        return "VERSION_NOT_ALLOWED";
    case ErrorCode::UnknownSigningKey:
        return "UNKNOWN_SIGNING_KEY";
    case ErrorCode::StaleVersion:
        return "STALE_VERSION";
    case ErrorCode::InvalidSetting:
        return "INVALID_SETTING";
    case ErrorCode::CsrInvalid:
        return "CSR_INVALID";
    case ErrorCode::KeyGeneration:
        return "KEY_GENERATION";
    case ErrorCode::Storage:
        return "STORAGE";
    }
    return "INTERNAL";
}

std::optional<ErrorCode> error_from_name(std::string_view name) noexcept
{
    constexpr auto kLast = static_cast<int>(ErrorCode::Storage);
    for (int i = 0; i <= kLast; ++i) {
        const auto code = static_cast<ErrorCode>(i);
        if (error_name(code) == name) {
            return code;
        }
    }
    return std::nullopt;
}

}  // namespace dextro::iot::procedures
