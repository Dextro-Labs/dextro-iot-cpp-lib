#pragma once

#include <dextro-iot/remote_link.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

/// O backend falso dos testes de `Keepalive` e `EventDispatcher`: guarda cada
/// chamada e deixa o teste responder quando quiser — na hora (`auto_answer`)
/// ou depois (`answer_next`), como o `Client` faria dentro de um `tick()`.
class FakeLink final : public dextro::iot::RemoteLink {
public:
    class Call {
    public:
        std::string op;
        nlohmann::json payload;
        std::int64_t now_ms = 0;
        std::int64_t timeout_ms = 0;
        RemoteDone done;
    };

    bool is_connected = true;
    int restarts = 0;
    std::vector<Call> calls;    ///< todas, na ordem
    std::deque<std::size_t> open;  ///< índices das sem resposta ainda
    /// Responde já, dentro do `call`, quando definido.
    std::function<dextro::iot::RemoteResult(const Call&)> auto_answer;

    bool connected() const noexcept override { return is_connected; }

    void call(std::string_view op, std::string_view payload_json, std::int64_t now_ms,
              std::int64_t timeout_ms, RemoteDone done) override
    {
        Call c;
        c.op = std::string(op);
        c.payload = nlohmann::json::parse(payload_json, nullptr, false);
        c.now_ms = now_ms;
        c.timeout_ms = timeout_ms;
        c.done = std::move(done);
        calls.push_back(std::move(c));
        if (auto_answer) {
            const dextro::iot::RemoteResult r = auto_answer(calls.back());
            RemoteDone d = std::move(calls.back().done);
            if (d) {
                d(r);
            }
            return;
        }
        open.push_back(calls.size() - 1);
    }

    void restart(std::int64_t) override { ++restarts; }

    /// Responde a mais antiga em aberto.
    void answer_next(int rescode, nlohmann::json payload = nlohmann::json::object(),
                     std::string error = {})
    {
        if (open.empty()) {
            return;
        }
        const std::size_t i = open.front();
        open.pop_front();
        dextro::iot::RemoteResult r;
        r.rescode = rescode;
        r.payload_json = payload.dump();
        r.error = std::move(error);
        RemoteDone d = std::move(calls[i].done);
        if (d) {
            d(r);
        }
    }

    static dextro::iot::RemoteResult result(int rescode, nlohmann::json payload = nlohmann::json::object(),
                                            std::string error = {})
    {
        dextro::iot::RemoteResult r;
        r.rescode = rescode;
        r.payload_json = payload.dump();
        r.error = std::move(error);
        return r;
    }
};
