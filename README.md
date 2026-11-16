# Dextro IoT — cliente C++ para Linux

Biblioteca C++17 que conecta um device Linux ao backend Dextro IoT
(`dextrolabs-device`, binding backend v0.5): MQTT sobre mTLS, device-procedures,
remote-procedures, keepalive de aplicação e `events-write`. Sem Qt, sem threads
próprias, sem banco de dados.

O código é a pilha que roda em campo no `ssat-traffic-controller`
(`src/drivers/iot/`), trazida para cá para existir **uma** cópia do protocolo.
Os comentários do código guardam as lições medidas em campo (4G que engole SYN,
NAT que expira, relógio que salta com o GNSS): leia antes de mudar.

## O que tem

| Peça | Header | O que faz |
|---|---|---|
| `Client` | `dextro-iot/iot_client.hpp` | A sessão inteira: TLS + MQTT, reconexão, despacho das device-procedures, `call()` de remote-procedure, `publish()` simples |
| `session::MqttSession` | `dextro-iot/session/mqtt_session.hpp` | A máquina de estados MQTT sobre o MQTT-C: CONNACK, PING próprio e monotônico, meia-conexão, LWT, presença retida |
| `session::TlsTransport` | `dextro-iot/session/tls_transport.hpp` | TCP + TLS mútuo (OpenSSL 3), não bloqueante, DNS assíncrono com cache, `TCP_USER_TIMEOUT`, sem SIGPIPE |
| `session::Backoff` | `dextro-iot/session/backoff.hpp` | Exponencial com full jitter, 1 s → 2 min, zera após 60 s estável |
| `session::LinkMonitor` | `dextro-iot/session/link_monitor.hpp` | Netlink: "a rede mudou" para reconectar na hora |
| `procedures::ProcedureTable` | `dextro-iot/procedures/procedure_table.hpp` | Registro das device-procedures por nome, com escopo bootstrap e `outbox_only` |
| `procedures::Procedure` | `dextro-iot/procedures/procedure.hpp` | A classe base: `validate()` + `execute()`; `handle()` nunca lança |
| ops genéricas | `dextro-iot/procedures/ops/*.hpp` | `list-procedures`, `get-status`, `settings-read`, `settings-write`, `outbox-notification`, `certificate-request`, `certificate-install` |
| `FileCertificateStore` | `dextro-iot/certificates/certificate_store.hpp` | Par EC P-256, CSR (CN = SN), instalação atômica do certificado operacional |
| `Keepalive` | `dextro-iot/keepalive.hpp` | A remote-procedure `keepalive` (§4.4), `nextIntervalMs`, reabre a sessão quando o service some |
| `EventDispatcher` + `EventStore` | `dextro-iot/event_dispatcher.hpp` | `events-write` (§4.5) em blocos sobre uma fila **da aplicação** |
| `log` | `dextro-iot/log.hpp` | Gancho de log; padrão stderr com prefixo `<N>` do journald |
| `clock` | `dextro-iot/clock.hpp` | `monotonic_ms()` (todo `now_ms` da API) e `wall_ms()` |

Tópicos (SPEC do dextrolabs-device §4.1): `atrio/{sn}/status` (LWT
`{"online":false}` e `{"online":true,"ts"}` retidos), `atrio/{sn}/procedure`,
`atrio/{sn}/procedure/reply/{instance}`, `atrio/{sn}/service`,
`atrio/{sn}/service/reply`, e `publish("live", ...)` para `atrio/{sn}/live`.

## O que NÃO tem (é da aplicação)

- **A fila de eventos e o `seq`.** A biblioteca não embute SQLite: a aplicação
  implementa `EventStore` (numa aplicação C++ da Dextro, entidade e repositório
  do MoleculaEntity) e persiste o `seq` entre reboots.
- **`ota-notification`.** Formatos de artefato, chaves de assinatura e o agente
  que baixa e instala são de cada produto. A aplicação escreve a op e a registra.
- **A outbox** (`outbox-read`/`outbox-ack`). A op `outbox-notification` só
  avisa a aplicação (`SessionControl::request_outbox_drain`); quem drena é ela,
  com `Client::call()`.
- **A política de credencial** (voltar ao certificado de fábrica depois de N
  recusas). A biblioteca expõe `Client::unauthorized_streak()`; a aplicação
  decide e chama `start()` com o outro par.
- **A leitura da configuração** (arquivo `.ini`, partição de fábrica): a
  aplicação preenche `Config`.

## Build

```bash
git clone --recursive git@github.com:Dextro-Labs/dextro-iot-cpp-lib.git
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build -j4
ctest --test-dir build -j2 --output-on-failure
```

Dependências: OpenSSL 3, pthreads, glibc (`getaddrinfo_a`). Submodules:
`external/mqtt-c` (MQTT-C) e `external/nlohmann-json` (usado quando o
`nlohmann_json` do sistema não é achado). Os testes usam o GoogleTest do
sistema, ou o baixam por FetchContent. Como subdiretório de outro projeto,
testes e exemplo ficam desligados por padrão.

```cmake
add_subdirectory(lib/dextro-iot-cpp-lib)
target_link_libraries(meu-daemon PRIVATE dextro::iot)
```

## Uso

Uma thread, um laço: `tick()` de cada peça e `poll()` no descritor do
`Client`. Nada é thread-safe; todo `now_ms` é de `clock::monotonic_ms()`.

### 1. Configuração mTLS e cliente

```cpp
#include <dextro-iot/iot_client.hpp>

dextro::iot::Config config;
config.host = "mqtt.dextrolabs.com.br";
config.port = 8883;
config.serial = dextro::iot::canonical_serial("a1:b2:c3:d4:e5:f6");  // "A1B2C3D4E5F6"
config.ca_cert = "/data/iot/ca.pem";
config.client_cert = "/data/iot/device.crt";   // CN = SN (SPEC §3.1)
config.client_key = "/data/iot/device.key";

dextro::iot::Client client;
// start() recusa (false) SN fora de ^[0-9A-F]{12}$ ou sem CA/cert/chave.
```

O clientId MQTT é o SN; o username não é enviado — o EMQX o tira do CN do
certificado (`mqtt.peer_cert_as_username = cn`, SPEC §5), e o hook de auth
exige CN = username = clientId. O servidor é conferido contra a CA e o nome do
host. Não existe modo sem TLS.

### 2. Registrar device-procedures

```cpp
#include <dextro-iot/procedures/ops/list_procedures.hpp>
#include <dextro-iot/procedures/ops/certificate_install.hpp>
#include <dextro-iot/procedures/ops/certificate_request.hpp>
#include <dextro-iot/procedures/procedure_table.hpp>

namespace procs = dextro::iot::procedures;

class OpenDoor final : public procs::Procedure {
public:
    std::optional<procs::Reply> validate(const procs::Json& p) const override
    {
        if (auto r = procs::only_keys(p, {"door"})) return r;   // additionalProperties: false
        if (auto r = procs::require(p, "door")) return r;
        return procs::integer_field(p, "door", 1);
    }
    procs::Reply execute(const procs::Request& rq) override
    {
        return procs::Reply::ok({{"opened", rq.payload["door"]}});
        // erro: procs::Reply::error(procs::Rescode::Conflict, "DOOR_JAMMED", "detalhe")
    }
};

procs::ProcedureTable table;
MySessionControl control;   // implementa procs::SessionControl
dextro::iot::FileCertificateStore certs(config.serial,
    {"/data/iot/pending", config.client_cert, config.client_key});

//         op                     versão boot   timeout outbox_only
table.add({"list-procedures",     1,     false, 30000,  false},
          std::make_unique<procs::ListProcedures>(table, "meu-tipo-de-device", 1));
table.add({"certificate-request", 1,     true,  120000, false},
          std::make_unique<procs::CertificateRequest>(&certs));
table.add({"certificate-install", 1,     true,  60000,  false},
          std::make_unique<procs::CertificateInstall>(&certs, &control));
table.add({"open-door",           1,     false, 15000,  false}, std::make_unique<OpenDoor>());

client.bind(table, control);
```

O `Client` decodifica o envelope v0.5 (versão, SN, TTL de 60 s com relógio
sincronizado, `CLOCK_SKEW`), despacha e publica a resposta. Op desconhecida é
404 `UNKNOWN_OP`; no escopo bootstrap só passam as linhas `bootstrap`; payload
acima de 256 KB é 413. `certificate-install` e `settings-write` pedem a
reconexão pelo `SessionControl`: a aplicação chama `client.restart(now)` **depois**
do `tick()` em que a resposta saiu.

### 3. Keepalive

```cpp
#include <dextro-iot/keepalive.hpp>

dextro::iot::Keepalive keepalive(client, [&] {
    dextro::iot::Keepalive::Status s;
    s.fw_version = "1.4.2";
    s.settings_version = settings.version();
    s.cert_fingerprint = dextro::iot::certificate_fingerprint(config.client_cert);
    s.extra = {{"rssi", -61}, {"tempC", 41.5}};   // só os campos do §4.4
    s.custom = {{"printheadC", 55}};              // o que é do tipo vai aqui
    return s;
}, dextro::iot::clock::monotonic_ms());
keepalive.on_reply([](const nlohmann::json& r) { /* settingsResync, pendingOutbox... */ });
```

Sai um na abertura de cada sessão (é o que ativa um certificado recém-instalado)
e depois a cada `intervalMs`. Duas faltas seguidas (504/5xx) reabrem a sessão.

### 4. Eventos com a fila da aplicação

```cpp
#include <dextro-iot/event_dispatcher.hpp>

class MyEventStore final : public dextro::iot::EventStore {
    // due(), acknowledge(ackSeq, seqs), reject(), reschedule(), next_touch()
    // sobre a tabela da aplicação. acknowledge() tira TUDO com seq <= ackSeq.
};

MyEventStore store;
dextro::iot::EventDispatcher events(client, store);
// ao gravar um evento novo na fila:
events.kick();
```

Blocos de até 200 eventos e 256 KB; drena ao conectar, a cada minuto e depois
de `kick()`. 503 não penaliza (nada é reagendado); 400/413 dividem o bloco ao
meio até isolar o evento ruim; `UNKNOWN_EVENT_TYPE`/`INVALID_ARGS` vão para
`reject()`; `CLOCK_SKEW`, 5xx e 504 voltam com backoff (30 s · 2ⁿ, até 15 min).

### 5. Log

```cpp
#include <dextro-iot/log.hpp>

dextro::iot::log::set_level(dextro::iot::log::Level::Debug);
dextro::iot::log::set_sink([](dextro::iot::log::Level lvl, const char* line, void*) {
    my_logger(static_cast<int>(lvl), line);
});
```

### 6. O laço

```cpp
dextro::iot::session::LinkMonitor network;
network.open();
client.start(config, dextro::iot::clock::monotonic_ms());

for (;;) {
    const auto now = dextro::iot::clock::monotonic_ms();
    int wait = client.tick(now);
    keepalive.tick(now);
    events.tick(now);
    for (auto due : {keepalive.next_in(now), events.next_in(now)})
        if (due >= 0 && due < wait) wait = static_cast<int>(due);

    pollfd fds[2] = {{client.fd(), short(POLLIN | (client.wants_write() ? POLLOUT : 0)), 0},
                     {network.fd(), POLLIN, 0}};
    poll(fds, 2, wait);
    if ((fds[1].revents & POLLIN) && network.drain())
        client.network_changed(dextro::iot::clock::monotonic_ms());
}
```

Remote-procedure avulsa: `client.call("inbox-write", json, now, 0, [](const dextro::iot::RemoteResult& r) { ... })`
— o `done` roda sempre, uma vez: resposta do backend, 503 sem sessão, 504 no
prazo (30 s), 413 ou 429 locais.

O exemplo completo, compilável, está em `examples/main.cpp`.

## Origem

Portado de `Dextro-SSAT/ssat-traffic-controller` (`origin/devel`, 67a0f77):
`src/drivers/iot/` (sessão, TLS, PAL do MQTT-C, procedures, certificados,
arquivo atômico), o keepalive de `src/services/iot-manager/iot_controller.cpp`
e o despachante de `src/services/event-manager/event_dispatcher.cpp`, com os
testes correspondentes. O que era do controlador de trânsito (enum de ops,
ports de anéis e programação, OTA, leitura do `ssat.ini`) ficou lá.
