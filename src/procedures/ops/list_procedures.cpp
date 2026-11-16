#include <dextro-iot/procedures/ops/list_procedures.hpp>

namespace dextro::iot::procedures {

ListProcedures::ListProcedures(const ProcedureTable& table, std::string device_type,
                               int contract_version)
    : table_(table), device_type_(std::move(device_type)), contract_version_(contract_version)
{
}

std::optional<Reply> ListProcedures::validate(const Json& payload) const
{
    // `maxProperties: 0`.
    return only_keys(payload, {});
}

Reply ListProcedures::execute(const Request& request)
{
    (void)request;
    Json procedures = Json::array();
    table_.for_each([&](const ProcedureSpec& line) {
        procedures.push_back({{"op", line.op}, {"version", line.version}});
    });
    return Reply::ok({{"deviceType", device_type_},
                      {"contractVersion", contract_version_},
                      {"procedures", std::move(procedures)}});
}

}  // namespace dextro::iot::procedures
