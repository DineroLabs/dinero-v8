#include "orchard_wallet.h"
#include "consensus/coin_type.h"
#include <openssl/crypto.h>
#include <cstddef>

namespace dinero::orchard {
namespace {
void Check(int32_t status) { if(status!=DINERO_ORCHARD_OK)throw BackendError(status); }
struct CleanViewingBytes {
    FullViewingKeyBytes& bytes;
    ~CleanViewingBytes(){OPENSSL_cleanse(bytes.data(),bytes.size());}
};
static_assert(consensus::DINERO_COIN_TYPE==1448);
static_assert(sizeof(DineroOrchardAddressText)==100);
static_assert(offsetof(DineroOrchardAddressText,text)==4);
}
void WalletKeys::Deleter::operator()(DineroOrchardWalletKeys* handle)const noexcept {
    if(dinero_orchard_wallet_free_v1(handle)!=DINERO_ORCHARD_OK)std::terminate();
}
WalletKeys WalletKeys::FromSeed(std::span<const uint8_t> seed,uint32_t account) {
    if(dinero_orchard_wallet_coin_type_v1()!=consensus::DINERO_COIN_TYPE)
        throw BackendError(DINERO_ORCHARD_FORMAT);
    DineroOrchardWalletKeys* handle=nullptr;
    Check(dinero_orchard_wallet_keys_v1(seed.data(),seed.size(),account,&handle));
    return WalletKeys(handle);
}
FullViewingKeyBytes WalletKeys::ExportFullViewingKey()const {
    FullViewingKeyBytes bytes{};Check(dinero_orchard_wallet_fvk_v1(handle_.get(),bytes.data()));return bytes;
}
WalletReceiver WalletKeys::Receiver(WalletScope scope,const DiversifierIndex& index)const {
    auto fvk=ExportFullViewingKey();const CleanViewingBytes clean{fvk};
    return WalletReceiver::FromViewingKey(fvk,scope,index);
}
WalletReceiver WalletReceiver::FromViewingKey(const FullViewingKeyBytes& fvk,
    WalletScope scope,const DiversifierIndex& index) {
    std::array<uint8_t,43> raw{};
    Check(dinero_orchard_wallet_receiver_v1(fvk.data(),uint8_t(scope),index.data(),raw.data()));
    return WalletReceiver(raw);
}
WalletReceiver WalletReceiver::DecodeAddress(std::string_view text,WalletNetwork network) {
    std::array<uint8_t,43> raw{};
    Check(dinero_orchard_address_decode_v1(reinterpret_cast<const uint8_t*>(text.data()),text.size(),uint8_t(network),raw.data()));
    return WalletReceiver(raw);
}
std::string WalletReceiver::EncodeAddress(WalletNetwork network)const {
    DineroOrchardAddressText text{};Check(dinero_orchard_address_encode_v1(raw_.data(),uint8_t(network),&text));
    if(text.length>90)throw BackendError(DINERO_ORCHARD_FORMAT);
    return {reinterpret_cast<const char*>(text.text),text.length};
}
} // namespace dinero::orchard
