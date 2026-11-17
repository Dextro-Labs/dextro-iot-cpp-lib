// Origem: tests/unit/iot_credential_policy_test.cpp do ssat-traffic-controller
// (origin/devel 67a0f77), sem mudança de caso.
#include <dextro-iot/credential_policy.hpp>

#include <gtest/gtest.h>

namespace {

using dextro::iot::CredentialPolicy;
using Scope = CredentialPolicy::Scope;

CredentialPolicy::Inputs fora(Scope scope, int streak, std::int64_t now_ms)
{
    CredentialPolicy::Inputs in;
    in.current = scope;
    in.unauthorized_streak = streak;
    in.connected = false;
    in.has_operational = true;
    in.has_bootstrap = true;
    in.now_ms = now_ms;
    return in;
}

}  // namespace

TEST(Credencial, TresRecusasNoOperacionalVaoParaOFabrica)
{
    CredentialPolicy p;
    EXPECT_FALSE(p.decide(fora(Scope::Operational, 0, 0)));
    EXPECT_FALSE(p.decide(fora(Scope::Operational, 2, 1000)));
    EXPECT_EQ(p.decide(fora(Scope::Operational, 3, 2000)), Scope::Bootstrap);
}

TEST(Credencial, RecusaPassageiraNaoPrendeNoFabrica)
{
    // O hook do EMQX fora do ar: CONNACK 5 para tudo. Vai para o de fábrica e,
    // recusado lá também, VOLTA ao operacional — nunca fica preso.
    CredentialPolicy p;
    ASSERT_EQ(p.decide(fora(Scope::Operational, 3, 0)), Scope::Bootstrap);
    p.switched(3, 0);
    EXPECT_FALSE(p.decide(fora(Scope::Bootstrap, 5, 1000)));
    EXPECT_EQ(p.decide(fora(Scope::Bootstrap, 6, 2000)), Scope::Operational);
    p.switched(6, 2000);
    // E alterna de novo, indefinidamente, enquanto o broker recusar.
    EXPECT_EQ(p.decide(fora(Scope::Operational, 9, 3000)), Scope::Bootstrap);
}

TEST(Credencial, DezMinutosSemConectarNoFabricaTentamOOperacional)
{
    CredentialPolicy p;
    p.switched(0, 0);
    EXPECT_FALSE(p.decide(fora(Scope::Bootstrap, 0, CredentialPolicy::kBootstrapMaxMs - 1)));
    EXPECT_EQ(p.decide(fora(Scope::Bootstrap, 0, CredentialPolicy::kBootstrapMaxMs)),
              Scope::Operational);
}

TEST(Credencial, ConectadoNaoTrocaEZeraAsContas)
{
    CredentialPolicy p;
    auto in = fora(Scope::Operational, 2, 0);
    in.connected = true;
    EXPECT_FALSE(p.decide(in));
    // O cliente zera a sequência ao conectar; depois de cair, conta do zero.
    EXPECT_FALSE(p.decide(fora(Scope::Operational, 2, 1000)));
    EXPECT_EQ(p.decide(fora(Scope::Operational, 3, 2000)), Scope::Bootstrap);
}

TEST(Credencial, SemOCertificadoDestinoFicaNoQueExiste)
{
    CredentialPolicy p;
    auto sem_fabrica = fora(Scope::Operational, 10, 0);
    sem_fabrica.has_bootstrap = false;
    EXPECT_FALSE(p.decide(sem_fabrica));

    CredentialPolicy q;
    auto sem_operacional = fora(Scope::Bootstrap, 10, CredentialPolicy::kBootstrapMaxMs * 2);
    sem_operacional.has_operational = false;
    EXPECT_FALSE(q.decide(sem_operacional));
}

TEST(Credencial, ConectadoNoFabricaReiniciaOPrazoDosDezMinutos)
{
    CredentialPolicy p;
    p.switched(0, 0);
    auto in = fora(Scope::Bootstrap, 0, 5 * 60 * 1000);
    in.connected = true;
    EXPECT_FALSE(p.decide(in));
    // Caiu aos 5 min: o prazo conta de quando esteve conectado.
    EXPECT_FALSE(p.decide(
        fora(Scope::Bootstrap, 0, 5 * 60 * 1000 + CredentialPolicy::kBootstrapMaxMs - 1)));
    EXPECT_EQ(p.decide(fora(Scope::Bootstrap, 0, 5 * 60 * 1000 + CredentialPolicy::kBootstrapMaxMs)),
              Scope::Operational);
}
