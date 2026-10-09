#include "orchard_wallet.h"
#include <openssl/crypto.h>
#include <utility>

namespace dinero::orchard {
namespace {
void Check(bool condition){if(!condition)throw std::runtime_error("Orchard wallet storage integrity, transaction or I/O failure");}
}
WalletStateBytes::WalletStateBytes(std::span<const uint8_t> bytes){Check(bytes.size()<=WalletStateBytes::kMaxBytes);bytes_.assign(bytes.begin(),bytes.end());}
WalletStateBytes::WalletStateBytes(size_t size){Check(size<=WalletStateBytes::kMaxBytes);bytes_.resize(size);}
void WalletStateBytes::Wipe()noexcept{if(!bytes_.empty())OPENSSL_cleanse(bytes_.data(),bytes_.size());}
WalletStateBytes::~WalletStateBytes(){Wipe();}
WalletStateBytes::WalletStateBytes(WalletStateBytes&& other)noexcept:bytes_(std::move(other.bytes_)){}
WalletStateBytes& WalletStateBytes::operator=(WalletStateBytes&& other)noexcept{if(this!=&other){Wipe();bytes_=std::move(other.bytes_);}return *this;}
} // namespace dinero::orchard
