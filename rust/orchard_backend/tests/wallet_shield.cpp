#include "orchard_wallet.h"
#include <algorithm>
#include <iostream>
#include <type_traits>
using namespace dinero::orchard;
static void Check(bool ok){if(!ok)throw std::runtime_error("Orchard shield construction test failed");}
template<class F>static void Reject(F fn){bool rejected=false;try{fn();}catch(const BackendError&){rejected=true;}Check(rejected);}
int main(){try{
    static_assert(!std::is_copy_constructible_v<WalletShieldPlan>);
    static_assert(std::is_nothrow_move_constructible_v<WalletShieldPlan>);
    const std::array<uint8_t,64> seed{7},recipientSeed{9};
    auto sender=WalletKeys::FromSeed(seed,0),receiver=WalletKeys::FromSeed(recipientSeed,0);
    std::vector<WalletPayment> payments{{5000,receiver.Receiver(WalletScope::External,{})}};
    payments[0].memo.fill(42);
    auto plan=WalletShieldPlan::Prepare(sender,payments);
    Check(plan.UnprovedFacts().value_balance==-5000);
    auto wrongPlan=WalletShieldPlan::Prepare(sender,payments);
    Check(!std::equal(std::begin(plan.UnprovedFacts().effect),std::end(plan.UnprovedFacts().effect),std::begin(wrongPlan.UnprovedFacts().effect)));
    SigningDomain domain;domain.network_code=2;domain.genesis_wire[0]=42;domain.branch_id=1;
    ResolvedInput input;input.txid_wire[0]=1;input.amount_una=10000;input.sequence=0xfffffffe;input.script_pub_key={0x51};
    std::vector<TransparentOutput> outputs{{4900,{0x51}}};
    const auto context=SigningContext::Create(domain,0,{input},outputs,100);
    const auto wrongBalance=SigningContext::Create(domain,0,{input},outputs,101);
    Reject([&]{(void)std::move(wrongPlan).Prove(wrongBalance);});
    Reject([&]{(void)std::move(wrongPlan).Prove(context);});
    auto complete=std::move(plan).Prove(context);
    Check(!complete.Bytes().empty());Check(complete.Authorization().Facts().value_balance==-5000);
    auto parsed=ParsedBundle::Decode(complete.Bytes());
    Check(parsed.VerifyAuthorization(context).SigningDigest()==complete.Authorization().SigningDigest());
    Reject([&]{(void)std::move(plan).Prove(context);});
    auto changed=input;changed.txid_wire[1]=1;
    Reject([&]{(void)parsed.VerifyAuthorization(SigningContext::Create(domain,0,{changed},outputs,100));});
    domain.network_code=1;
    Reject([&]{(void)parsed.VerifyAuthorization(SigningContext::Create(domain,0,{input},outputs,100));});
    std::cout<<"Fresh cross-address shield proof, owned-context signing, balance gate, one-use plan and independent verification passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
