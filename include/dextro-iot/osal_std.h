#ifndef DEXTRO_IOT_OSAL_STD_H
#define DEXTRO_IOT_OSAL_STD_H

#include "dextro-iot/osal.h"

#include <memory>

namespace dextro {
namespace iot {

/// Implementação padrão do IOSAL sobre <thread> e <mutex>.
///
/// A classe concreta vivia só no .cpp, sem header: o README mandava incluir
/// <dextro-iot/osal_std.h> e esse arquivo não existia, então nenhum consumidor
/// conseguia construir o OSAL padrão — a biblioteca não dava para usar.
///
/// A fábrica devolve a interface em vez de expor a classe para o tipo concreto
/// poder mudar sem quebrar quem linka.
std::shared_ptr<IOSAL> make_std_osal();

}  // namespace iot
}  // namespace dextro

#endif
