#include <dextro-iot/procedures/remote_call.hpp>

#include <dextro-iot/procedures/envelope.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace dextro::iot::procedures {
namespace {

using Json = nlohmann::json;

void finish(RemoteCalls::Done& done, int rescode, std::string_view why)
{
    if (!done) {
        return;
    }
    RemoteReply reply;
    reply.rescode = rescode;
    reply.error = std::string(why);
    done(reply);
}

}  // namespace

std::string RemoteCalls::begin(const Request& request, Done done)
{
    if (pending_.size() >= kMaxPending) {
        finish(done, 429, "remote-procedures demais em voo");
        return {};
    }
    // O envelope da spec v0.5 §2: metadata + op + payload. `instance` é o
    // clientId do device, que no MQTT é o próprio serialNumber.
    const Json envelope = {
        {"metadata",
         {{"v", std::string(kProtocolVersion)},
          {"messageId", request.message_id},
          {"relationId", request.relation_id},
          {"timestamp", request.wall_ms},
          {"serialNumber", request.serial},
          {"instance", request.serial}}},
        {"op", request.op},
        {"payload", request.payload.is_null() ? Json::object() : request.payload},
    };
    Pending p;
    p.op = request.op;
    p.deadline_ms = request.now_ms + std::max<std::int64_t>(request.timeout_ms, 1);
    p.done = std::move(done);
    pending_[request.relation_id] = std::move(p);
    return envelope.dump();
}

void RemoteCalls::cancel(const std::string& relation_id, int rescode, std::string_view why)
{
    const auto it = pending_.find(relation_id);
    if (it == pending_.end()) {
        return;
    }
    Done done = std::move(it->second.done);
    pending_.erase(it);
    finish(done, rescode, why);
}

bool RemoteCalls::on_reply(std::string_view text)
{
    const Json msg = Json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!msg.is_object()) {
        return false;
    }
    const auto meta = msg.find("metadata");
    if (meta == msg.end() || !meta->is_object()) {
        return false;
    }
    const auto rel = meta->find("relationId");
    if (rel == meta->end() || !rel->is_string()) {
        return false;
    }
    const auto it = pending_.find(rel->get<std::string>());
    if (it == pending_.end()) {
        return false;  // atrasada (já venceu) ou de outro pedido
    }
    Done done = std::move(it->second.done);
    pending_.erase(it);

    RemoteReply reply;
    const auto code = msg.find("rescode");
    reply.rescode = code != msg.end() && code->is_number_integer() ? code->get<int>() : 500;
    const auto body = msg.find("payload");
    if (body != msg.end() && !body->is_null()) {
        reply.payload = *body;
    }
    if (!reply.ok() && reply.payload.is_object()) {
        const auto err = reply.payload.find("error");
        if (err != reply.payload.end() && err->is_string()) {
            reply.error = err->get<std::string>();
        }
    }
    if (done) {
        done(reply);
    }
    return true;
}

void RemoteCalls::expire(std::int64_t now_ms)
{
    std::vector<Done> expired;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now_ms >= it->second.deadline_ms) {
            expired.push_back(std::move(it->second.done));
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
    for (Done& done : expired) {
        finish(done, 504, "sem resposta no prazo");
    }
}

void RemoteCalls::fail_all(int rescode, std::string_view why)
{
    std::map<std::string, Pending> all;
    all.swap(pending_);
    for (auto& [id, p] : all) {
        (void)id;
        finish(p.done, rescode, why);
    }
}

std::int64_t RemoteCalls::next_deadline_in(std::int64_t now_ms) const noexcept
{
    std::int64_t best = -1;
    for (const auto& [id, p] : pending_) {
        (void)id;
        const std::int64_t left = std::max<std::int64_t>(p.deadline_ms - now_ms, 0);
        best = best < 0 ? left : std::min(best, left);
    }
    return best;
}

}  // namespace dextro::iot::procedures
