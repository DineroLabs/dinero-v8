#include "orchard_wallet.h"
#include <algorithm>
#include <iostream>
#include <type_traits>
using namespace dinero::orchard;
static void Check(bool ok){if(!ok)throw std::runtime_error("Orchard wallet key/address test failed");}
template<class F>static void Reject(F fn){bool rejected=false;try{fn();}catch(const BackendError&){rejected=true;}Check(rejected);}
int main(){try{
    static_assert(!std::is_copy_constructible_v<WalletKeys>);
    static_assert(std::is_nothrow_move_constructible_v<WalletKeys>);
    const std::array<uint8_t,64> seed{7};auto keys=WalletKeys::FromSeed(seed,0);
    auto same=WalletKeys::FromSeed(seed,0),other=WalletKeys::FromSeed(seed,1);
    const auto fvk=keys.ExportFullViewingKey();Check(fvk==same.ExportFullViewingKey());Check(fvk!=other.ExportFullViewingKey());
    DiversifierIndex zero{},last{};last[10]=1;
    const auto receive=keys.Receiver(WalletScope::External,zero),change=keys.Receiver(WalletScope::Internal,zero);
    Check(receive!=change);Check(receive!=keys.Receiver(WalletScope::External,last));
    Check(WalletReceiver::FromViewingKey(fvk,WalletScope::External,zero)==receive);
    for(const auto network:{WalletNetwork::Mainnet,WalletNetwork::Testnet,WalletNetwork::Regtest}) {
        const auto address=receive.EncodeAddress(network);Check(address.size()<=90);
        Check(WalletReceiver::DecodeAddress(address,network)==receive);
        auto upper=address;std::transform(upper.begin(),upper.end(),upper.begin(),[](unsigned char c){return std::toupper(c);});
        Check(WalletReceiver::DecodeAddress(upper,network)==receive);
        Reject([&]{(void)WalletReceiver::DecodeAddress(address,WalletNetwork((uint8_t(network)+1)%3));});
        auto bad=address;bad.back()=bad.back()=='q'?'p':'q';Reject([&]{(void)WalletReceiver::DecodeAddress(bad,network);});
        Reject([&]{(void)WalletReceiver::DecodeAddress(address+"q",network);});
    }
    Reject([&]{(void)WalletKeys::FromSeed(std::span<const uint8_t>(seed).first(31),0);});
    Reject([&]{(void)WalletKeys::FromSeed(seed,0x80000000);});
    Reject([&]{(void)keys.Receiver(WalletScope(255),zero);});
    Reject([&]{(void)receive.EncodeAddress(WalletNetwork(255));});
    FullViewingKeyBytes invalid{};invalid.fill(0xff);
    Reject([&]{(void)WalletReceiver::FromViewingKey(invalid,WalletScope::External,zero);});
    auto moved=std::move(keys);Check(moved.ExportFullViewingKey()==fvk);
    Reject([&]{(void)keys.ExportFullViewingKey();});
    std::cout<<"Orchard wallet: ZIP32 account/scope/index separation, watch-only parity, explicit network and strict address checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
