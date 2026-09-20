#ifndef DEXTRO_IOT_NETWORK_POSIX_H
#define DEXTRO_IOT_NETWORK_POSIX_H

#include "dextro-iot/network.h"

#include <memory>

namespace dextro {
namespace iot {

/// Provedor de rede POSIX (sockets BSD).
///
/// ⚠️ **Não faz TLS.** `create_tls_socket()` devolve um socket TCP puro, sem
/// erro e sem aviso — pediu TLS, recebeu texto claro. Quem autentica por mTLS
/// NÃO deve usar este provedor até o TLS existir; implemente o
/// INetworkProvider por conta própria, ou trate o retorno como inseguro.
std::shared_ptr<INetworkProvider> make_posix_network();

}  // namespace iot
}  // namespace dextro

#endif
