// Exemplo mínimo: um device que conecta com mTLS, atende as device-procedures
// genéricas mais uma op do tipo, manda keepalive e drena eventos.
//
//   iot-example <host> <SN> <ca.pem> <cert.pem> <key.pem>
//
// A fila de eventos aqui é em memória só para o exemplo caber num arquivo: num
// device de verdade ela é durável (uma tabela do MoleculaEntity numa aplicação
// C++ da Dextro), com o `seq` persistido entre reboots.
#include <dextro-iot/certificates/certificate_store.hpp>
#include <dextro-iot/clock.hpp>
#include <dextro-iot/event_dispatcher.hpp>
#include <dextro-iot/iot_client.hpp>
#include <dextro-iot/keepalive.hpp>
#include <dextro-iot/log.hpp>
#include <dextro-iot/procedures/ops/list_procedures.hpp>
#include <dextro-iot/procedures/procedure_table.hpp>
#include <dextro-iot/session/link_monitor.hpp>

#include <poll.h>

#include <csignal>
#include <cstdio>
#include <map>
#include <memory>

namespace iot = dextro::iot;
namespace procs = dextro::iot::procedures;

namespace {

volatile std::sig_atomic_t g_stop = 0;

/// Uma op do tipo de device: `{"door": 1..4}` → abre a porta.
class OpenDoor final : public procs::Procedure {
public:
    std::optional<procs::Reply> validate(const procs::Json& payload) const override
    {
        if (auto refused = procs::only_keys(payload, {"door"})) {
            return refused;
        }
        if (auto refused = procs::require(payload, "door")) {
            return refused;
        }
        return procs::integer_field(payload, "door", 1);
    }
    procs::Reply execute(const procs::Request& request) override
    {
        return procs::Reply::ok({{"opened", request.payload["door"]}});
    }
};

/// A sessão vista pelas ops: aqui, só o pedido de reconexão.
class Control final : public procs::SessionControl {
public:
    bool reconnect = false;
    void request_outbox_drain() override {}
    void request_reconnect() override { reconnect = true; }
    bool bootstrap_scope() const override { return false; }
};

/// SÓ PARA O EXEMPLO: uma fila em memória perde tudo no reboot.
class MemoryEvents final : public iot::EventStore {
public:
    std::map<std::int64_t, iot::StoredEvent> rows;
    std::int64_t next_seq = 1;

    std::vector<iot::StoredEvent> due(std::int64_t, std::size_t limit) override
    {
        std::vector<iot::StoredEvent> out;
        for (const auto& kv : rows) {
            if (out.size() == limit) {
                break;
            }
            out.push_back(kv.second);
        }
        return out;
    }
    std::size_t acknowledge(std::int64_t ack, const std::vector<std::int64_t>& seqs) override
    {
        std::size_t n = 0;
        for (auto it = rows.begin(); it != rows.end();) {
            bool listed = false;
            for (auto s : seqs) {
                listed = listed || s == it->first;
            }
            if (it->first <= ack || listed) {
                it = rows.erase(it);
                ++n;
            } else {
                ++it;
            }
        }
        return n;
    }
    void reject(std::int64_t seq, std::string_view) override { rows.erase(seq); }
    void reschedule(const std::vector<std::int64_t>&, std::int64_t, std::string_view, bool) override {}
    std::optional<std::int64_t> next_touch() override { return std::nullopt; }
};

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 6) {
        std::fprintf(stderr, "uso: %s <host> <SN> <ca.pem> <cert.pem> <key.pem>\n", argv[0]);
        return 2;
    }
    std::signal(SIGINT, [](int) { g_stop = 1; });
    iot::log::set_level(iot::log::Level::Info);

    iot::Config config;
    config.host = argv[1];
    config.serial = iot::canonical_serial(argv[2]);
    config.ca_cert = argv[3];
    config.client_cert = argv[4];
    config.client_key = argv[5];

    procs::ProcedureTable table;
    Control control;
    table.add({"list-procedures", 1, false, 30000, false},
              std::make_unique<procs::ListProcedures>(table, "example-device", 1));
    table.add({"open-door", 1, false, 15000, false}, std::make_unique<OpenDoor>());

    iot::Client client;
    client.bind(table, control);

    const std::int64_t started = iot::clock::monotonic_ms();
    iot::Keepalive keepalive(client,
                             [&] {
                                 iot::Keepalive::Status s;
                                 s.fw_version = "0.0.1";
                                 s.cert_fingerprint = iot::certificate_fingerprint(config.client_cert);
                                 return s;
                             },
                             started);

    MemoryEvents events;
    iot::EventDispatcher dispatcher(client, events);
    iot::StoredEvent boot;
    boot.seq = events.next_seq++;
    boot.code = "power-up";
    boot.args = "{}";
    boot.occurred_ms = iot::clock::wall_ms();
    events.rows[boot.seq] = boot;

    iot::session::LinkMonitor network;
    (void)network.open();

    if (!client.start(config, started)) {
        return 1;
    }
    while (g_stop == 0) {
        const std::int64_t now = iot::clock::monotonic_ms();
        int wait = client.tick(now);
        keepalive.tick(now);
        dispatcher.tick(now);
        if (control.reconnect) {
            control.reconnect = false;
            client.restart(now);
        }
        for (std::int64_t due : {keepalive.next_in(now), dispatcher.next_in(now)}) {
            if (due >= 0 && due < wait) {
                wait = static_cast<int>(due);
            }
        }

        pollfd fds[2] = {{client.fd(), static_cast<short>(POLLIN | (client.wants_write() ? POLLOUT : 0)), 0},
                         {network.fd(), POLLIN, 0}};
        (void)::poll(fds, 2, wait);
        if ((fds[1].revents & POLLIN) != 0 && network.drain()) {
            client.network_changed(iot::clock::monotonic_ms());
        }
    }
    client.stop(iot::clock::monotonic_ms());
    return 0;
}
