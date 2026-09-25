#include "orchard_wallet.h"
#include "orchard_transaction.h"
#include <algorithm>
#include <iostream>
using namespace dinero::orchard;
static void Check(bool ok){if(!ok)throw std::runtime_error("Orchard wallet spend lifecycle failed");}
template<class F>static void Reject(F fn){bool rejected=false;try{fn();}catch(const BackendError&){rejected=true;}Check(rejected);}
static std::vector<Hash> Commitments(const VerifiedAuthorization& auth){
    std::vector<Hash> out(auth.Facts().action_count);
    for(size_t i=0;i<out.size();++i)std::copy(std::begin(auth.Facts().commitments[i]),std::end(auth.Facts().commitments[i]),out[i].begin());return out;
}
static std::pair<WalletNote,size_t> Receive(const WalletKeys& keys,const ProvedWalletBundle& bundle){
    for(uint32_t i=0;i<bundle.Authorization().Facts().action_count;++i){
        auto note=WalletNote::Receive(bundle.Authorization(),keys.ExportFullViewingKey(),WalletScope::External,i);
        if(note)return {std::move(*note),i};
    }
    throw std::runtime_error("expected recipient note absent");
}
static Hash Root(const WalletWitness& witness){Hash out{};std::copy(std::begin(witness.Facts().root),std::end(witness.Facts().root),out.begin());return out;}
int main(){try{
    auto a=WalletKeys::FromSeed(std::array<uint8_t,64>{7},0);
    auto b=WalletKeys::FromSeed(std::array<uint8_t,64>{9},0);
    auto c=WalletKeys::FromSeed(std::array<uint8_t,64>{11},0);
    SigningDomain domain;domain.network_code=2;domain.genesis_wire[0]=42;domain.branch_id=1;
    ResolvedInput input;input.txid_wire[0]=1;input.amount_una=10000;input.sequence=0xfffffffe;input.script_pub_key={0x51};
    std::vector<WalletPayment> funding{{5000,b.Receiver(WalletScope::External,{})}};funding[0].memo.fill(42);
    auto shield=WalletBundlePlan::PrepareShield(a,funding).Prove(SigningContext::Create(domain,0,{input},{{4900,{0x51}}},100));
    auto [note,index]=Receive(b,shield);Check(note.Facts().amount==5000);Check(note.Facts().memo[0]==42);
    Check(!WalletNote::Receive(shield.Authorization(),c.ExportFullViewingKey(),WalletScope::External,uint32_t(index)));
    Check(!WalletNote::Receive(shield.Authorization(),b.ExportFullViewingKey(),WalletScope::Internal,uint32_t(index)));
    const auto empty=OrchardFrontier::Empty();const auto leaves=Commitments(shield.Authorization());
    const auto first=empty.Append(leaves);const auto witness=WalletWitness::ForAppendedLeaf(empty,leaves,index);
    Check(Root(witness)==first.Root());Check(witness.Facts().position==index);
    // Incremental updates are checked against both roots and preserve the old
    // witness, which the wallet can retain as an undo/checkpoint state.
    const std::vector<Hash> appended{Hash{13},Hash{17},Hash{19}};
    const auto second=first.Append(appended);
    auto updated=witness.Append(appended,first.Root(),second.Root());Check(Root(updated)==second.Root());
    Check(Root(witness)==first.Root());Check(updated.Facts().leaf_count==leaves.size()+3);
    Reject([&]{(void)witness.Append(appended,second.Root(),second.Root());});
    Reject([&]{(void)witness.Append(appended,first.Root(),first.Root());});
    std::vector<WalletSpendInput> spends{{note,updated}};
    std::vector<WalletPayment> outputs{{4900,c.Receiver(WalletScope::External,{})}};
    Reject([&]{(void)WalletBundlePlan::PrepareSpend(a,spends,second.Root(),outputs);});
    Reject([&]{(void)WalletBundlePlan::PrepareSpend(b,spends,first.Root(),outputs);});
    const std::vector<WalletSpendInput> duplicate{{note,updated},{note,updated}};
    Reject([&]{(void)WalletBundlePlan::PrepareSpend(b,duplicate,second.Root(),outputs);});
    auto transfer=WalletBundlePlan::PrepareSpend(b,spends,second.Root(),outputs).Prove(SigningContext::Create(domain,0,{}, {},100));
    Check(transfer.Authorization().Facts().value_balance==100);
    bool spent=false;for(uint32_t i=0;i<transfer.Authorization().Facts().action_count;++i)
        spent|=std::equal(std::begin(note.Facts().nullifier),std::end(note.Facts().nullifier),transfer.Authorization().Facts().nullifiers[i]);
    Check(spent);
    auto [received,receivedIndex]=Receive(c,transfer);Check(received.Facts().amount==4900);
    auto envelope=TransactionEnvelope::Create(0,{}, {},100,transfer.Bytes());
    Check(envelope.VerifyAuthorization(domain,{}).Orchard().SigningDigest()==transfer.Authorization().SigningDigest());
    auto transferLeaves=Commitments(transfer.Authorization());const auto third=second.Append(transferLeaves);
    auto receivedWitness=WalletWitness::ForAppendedLeaf(second,transferLeaves,receivedIndex);
    std::vector<WalletSpendInput> withdrawal{{received,receivedWitness}};
    std::vector<TransparentOutput> cash{{4800,{0x51}}};
    auto unshield=WalletBundlePlan::PrepareSpend(c,withdrawal,third.Root(),{}).Prove(SigningContext::Create(domain,0,{},cash,100));
    Check(unshield.Authorization().Facts().value_balance==4900);
    auto exit=TransactionEnvelope::Create(0,{},cash,100,unshield.Bytes());
    Check(exit.VerifyAuthorization(domain,{}).Orchard().SigningDigest()==unshield.Authorization().SigningDigest());
    for(uint32_t i=0;i<unshield.Authorization().Facts().action_count;++i)
        Check(!WalletNote::Receive(unshield.Authorization(),c.ExportFullViewingKey(),WalletScope::External,i));
    // Net transparent deposit 5000 is withdrawn as 4800 + two100 fees.
    Check(-shield.Authorization().Facts().value_balance==transfer.Authorization().Facts().value_balance+unshield.Authorization().Facts().value_balance);
    std::cout<<"Fresh shield -> recipient decrypt -> witnessed cross-address send -> unshield proof lifecycle passed; all amounts conserved\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
