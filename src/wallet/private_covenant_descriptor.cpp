#include "wallet/private_covenant_descriptor.h"
#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include <secp256k1.h>
#include <limits>
#include <algorithm>
#include <cstring>

namespace dinero::wallet {
namespace sh = consensus::shielded;
namespace {
constexpr std::array<uint8_t,8> magic{{'D','I','N','P','C','V',1,0}};
sh::Hash Derive(const sh::Hash& seed, uint8_t purpose, uint8_t index) {
    constexpr char domain[] = "DIN/private-covenant/material/v1";
    for (uint32_t counter = 0; ; ++counter) {
        const uint8_t suffix[]{purpose,index,uint8_t(counter),uint8_t(counter>>8),uint8_t(counter>>16),uint8_t(counter>>24)};
        sh::Hash result{};
        crypto::CSHA256().Write(reinterpret_cast<const uint8_t*>(domain),sizeof(domain)-1)
            .Write(seed.data(),seed.size()).Write(suffix,sizeof(suffix)).Finalize(result.data());
        if (secp256k1_ec_seckey_verify(crypto::GetSecp256k1ContextSignVerify(), result.data())) return result;
        if (counter == UINT32_MAX) throw std::runtime_error("covenant scalar derivation failed");
    }
}
void Write(std::array<uint8_t,512>& bytes, size_t& pos, uint64_t value, size_t count) {
    for(size_t i=0;i<count;++i) bytes.at(pos++) = uint8_t(value >> (8*i));
}
uint64_t Read(const std::array<uint8_t,512>& bytes,size_t& pos,size_t count) {
    uint64_t result=0;
    for(size_t i=0;i<count;++i) result |= uint64_t(bytes.at(pos++)) << (8*i);
    return result;
}
}
bool IsPrivateCovenantMemo(const std::array<uint8_t,512>& memo) {
    return std::equal(magic.begin(), magic.end(), memo.begin());
}
uint64_t PrivateCovenantFundingValue(const PrivateCovenantDescriptor& d) {
    if (d.outputs.empty() || d.outputs.size()>2 || d.seed==sh::Hash{} || !d.fee_una ||
        d.fee_una > uint64_t(INT64_MAX) || d.minimum_height==UINT32_MAX)
        throw std::invalid_argument("invalid private covenant descriptor");
    uint64_t value=d.fee_una;
    for(const auto& output:d.outputs) {
        if (!output.value_una || output.value_una > uint64_t(INT64_MAX)-value)
            throw std::invalid_argument("private covenant amount overflow");
        // Decode validates both recipient keys and the nullifier commitment.
        shielded::DecodeShieldedAddress(shielded::EncodeShieldedAddress(output.address, "rdins"));
        value += output.value_una;
    }
    return value;
}
std::array<uint8_t,512> EncodePrivateCovenantDescriptor(const PrivateCovenantDescriptor& d) {
    PrivateCovenantFundingValue(d);
    std::array<uint8_t,512> bytes{};
    std::copy(magic.begin(),magic.end(),bytes.begin()); size_t pos=magic.size();
    Write(bytes,pos,d.outputs.size(),1); Write(bytes,pos,d.minimum_height,4); Write(bytes,pos,d.fee_una,8);
    std::copy(d.seed.begin(),d.seed.end(),bytes.begin()+pos); pos+=d.seed.size();
    for(const auto& output:d.outputs) {
        std::copy(output.address.begin(),output.address.end(),bytes.begin()+pos); pos+=output.address.size();
        Write(bytes,pos,output.value_una,8);
    }
    return bytes;
}
std::optional<PrivateCovenantDescriptor> DecodePrivateCovenantDescriptor(const std::array<uint8_t,512>& bytes) {
    if(!IsPrivateCovenantMemo(bytes)) return std::nullopt;
    try {
        PrivateCovenantDescriptor d; size_t pos=magic.size();
        const auto count=Read(bytes,pos,1); if(count<1 || count>2) return std::nullopt;
        d.minimum_height=Read(bytes,pos,4); d.fee_una=Read(bytes,pos,8);
        std::copy_n(bytes.begin()+pos,d.seed.size(),d.seed.begin()); pos+=d.seed.size();
        for(size_t i=0;i<count;++i) {
            PrivateCovenantPayee output;
            std::copy_n(bytes.begin()+pos,output.address.size(),output.address.begin()); pos+=output.address.size();
            output.value_una=Read(bytes,pos,8); d.outputs.push_back(output);
        }
        if(EncodePrivateCovenantDescriptor(d)!=bytes) return std::nullopt; // canonical zero padding
        return d;
    } catch(const std::exception&) { return std::nullopt; }
}
std::vector<PrivateCovenantOutputMaterial> DerivePrivateCovenantOutputs(const PrivateCovenantDescriptor& d) {
    PrivateCovenantFundingValue(d);
    std::vector<PrivateCovenantOutputMaterial> result;
    for(size_t i=0;i<d.outputs.size();++i) {
        PrivateCovenantOutputMaterial material;
        material.recipient=shielded::DecodeShieldedAddress(shielded::EncodeShieldedAddress(d.outputs[i].address,"rdins"));
        material.value_una=d.outputs[i].value_una;
        material.rcm=Derive(d.seed,0,i); material.esk=Derive(d.seed,1,i);
        sh::Hash packed{},value{};
        std::copy(material.recipient.d.begin(),material.recipient.d.end(),packed.begin());
        for(unsigned j=0;j<8;++j) value[31-j]=uint8_t(material.value_una>>(8*j));
        const auto ownership=sh::AuthRecipientCommitmentKey(material.recipient.pk_d_spend,material.recipient.nfk_commitment);
        material.output.commitment=sh::NoteCommitment(packed,ownership,value,material.rcm);
        shielded::NotePlaintext note;
        note.d=material.recipient.d; note.value_una=material.value_una; note.rcm=material.rcm;
        const auto encrypted=shielded::EncryptNoteForRecipient(note.d,material.recipient.pk_d,note,&material.esk);
        material.output.encrypted_note.assign(encrypted.begin(),encrypted.end());
        result.push_back(std::move(material));
    }
    // The wire serializer sorts outputs by commitment even when the bundle
    // builder preserves insertion order. Commit to wire order now, otherwise
    // a two-payee contract fails only for seeds whose commitments reorder.
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b) {
        return a.output.commitment < b.output.commitment;
    });
    return result;
}
sh::Hash PrivateCovenantDescriptorRoot(const PrivateCovenantDescriptor& d) {
    std::vector<sh::ShieldedOutput> outputs;
    for(auto& material:DerivePrivateCovenantOutputs(d)) outputs.push_back(std::move(material.output));
    return sh::PrivateCovenantOutputRoot(outputs);
}
}
