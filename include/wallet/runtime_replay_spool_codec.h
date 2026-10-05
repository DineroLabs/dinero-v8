#pragma once
#include "daemon/runtime_block_outbox.h"
#include "wallet/runtime_replay_spool.h"
#include <algorithm>
#include <span>

namespace dinero::wallet::detail {
// Private raw-material codec. No sealed authorization/state classes are
// serialized. The caller still validates the source and every real proof.
namespace replay_spool_codec {
using Bytes=RuntimeReplaySpool::Bytes;
inline void Check(bool ok){if(!ok)throw consensus::OrchardStateLookupError(Status::Corruption);}
struct Writer {
    Bytes bytes;
    void Number(uint64_t value,unsigned size){Check(bytes.size()<=RuntimeReplaySpool::MaximumRecordBytes&&size<=RuntimeReplaySpool::MaximumRecordBytes-bytes.size());Check(size&&size<=8&&(size==8||!(value>>(8*size))));for(unsigned i=0;i<size;++i){bytes.push_back(uint8_t(value));value>>=8;}}
    void Raw(std::span<const uint8_t> value){Check(value.size()<=RuntimeReplaySpool::MaximumRecordBytes-bytes.size());bytes.insert(bytes.end(),value.begin(),value.end());}
    void Hash(const uint256& hash){Raw({hash.data,32});}
    void Blob(std::span<const uint8_t> value){Check(value.size()<=RuntimeReplaySpool::MaximumRecordBytes);Number(value.size(),4);Raw(value);}
    void State(const storage::OrchardStoredState& state){
        Number(state.height,4);Hash(state.block_hash);Hash(state.anchor);Number(state.pool_balance,8);Number(state.tree_size,8);
        Check(state.frontier.size()<=storage::ORCHARD_STORED_FRONTIER_LIMIT);
        Blob({reinterpret_cast<const uint8_t*>(state.frontier.data()),state.frontier.size()});
    }
};
struct Reader {
    std::span<const uint8_t> bytes;
    std::span<const uint8_t> Raw(size_t size){Check(size<=bytes.size());auto value=bytes.first(size);bytes=bytes.subspan(size);return value;}
    uint64_t Number(unsigned size){Check(size&&size<=8);const auto value=Raw(size);uint64_t result=0;for(unsigned i=0;i<size;++i)result|=uint64_t(value[i])<<(8*i);return result;}
    uint256 Hash(){const auto value=Raw(32);uint256 hash;std::copy(value.begin(),value.end(),hash.begin());return hash;}
    Bytes Blob(size_t maximum=RuntimeReplaySpool::MaximumRecordBytes){const auto size=Number(4);Check(size<=maximum);const auto value=Raw(size);return {value.begin(),value.end()};}
    storage::OrchardStoredState State(){
        storage::OrchardStoredState s;s.height=uint32_t(Number(4));s.block_hash=Hash();s.anchor=Hash();s.pool_balance=Number(8);s.tree_size=Number(8);
        const auto size=Number(4);Check(size<=storage::ORCHARD_STORED_FRONTIER_LIMIT);const auto value=Raw(size);
        s.frontier.assign(reinterpret_cast<const char*>(value.data()),value.size());return s;
    }
};
inline Bytes Encode(const RuntimeOutboxEvent& event){
    Writer w;w.Number(1,1);w.Number(event.cursor.sequence,8);w.Hash(event.cursor.digest);w.Hash(event.previous_digest);
    Check(event.direction==RuntimeBlockDirection::Connect||event.direction==RuntimeBlockDirection::Disconnect);
    w.Number(event.direction==RuntimeBlockDirection::Connect?1:2,1);
    const auto& c=event.context;w.Number(c.height,4);w.Hash(c.block_hash);w.Hash(c.parent_hash);w.Number(c.activation_height,4);
    w.Number(c.domain.network_code,1);w.Raw(c.domain.genesis_wire);w.Number(c.domain.branch_id,4);w.Blob(event.body);
    w.Number(event.orchard_replay.has_value(),1);
    if(event.orchard_replay){
        const auto& f=*event.orchard_replay;w.Number(f.parent.has_value(),1);if(f.parent)w.State(*f.parent);w.State(f.next);w.Blob(f.coin_undo);
        Check(f.branch_mtp.size()<=RuntimeReplaySpool::MaximumRecordBytes/12);w.Number(f.branch_mtp.size(),4);
        for(const auto& [height,time]:f.branch_mtp){w.Number(height,4);w.Number(time,8);}
    }
    Check(w.bytes.size()<=RuntimeReplaySpool::MaximumRecordBytes);return std::move(w.bytes);
}
inline RuntimeOutboxEvent Decode(std::span<const uint8_t> bytes){
    Check(bytes.size()<=RuntimeReplaySpool::MaximumRecordBytes);Reader r{bytes};Check(r.Number(1)==1);RuntimeOutboxEvent e;
    e.cursor.sequence=r.Number(8);e.cursor.digest=r.Hash();e.previous_digest=r.Hash();const auto direction=r.Number(1);Check(direction==1||direction==2);
    e.direction=direction==1?RuntimeBlockDirection::Connect:RuntimeBlockDirection::Disconnect;
    auto& c=e.context;c.height=uint32_t(r.Number(4));c.block_hash=r.Hash();c.parent_hash=r.Hash();c.activation_height=uint32_t(r.Number(4));
    c.domain.network_code=uint8_t(r.Number(1));const auto genesis=r.Raw(32);std::copy(genesis.begin(),genesis.end(),c.domain.genesis_wire.begin());c.domain.branch_id=uint32_t(r.Number(4));e.body=r.Blob();
    const auto replay=r.Number(1);Check(replay<=1);
    if(replay){
        RuntimeOrchardReplay f;const auto parent=r.Number(1);Check(parent<=1);if(parent)f.parent=r.State();f.next=r.State();f.coin_undo=r.Blob();
        const auto count=r.Number(4);Check(count<=r.bytes.size()/12);uint32_t previous=0;
        for(uint64_t i=0;i<count;++i){const auto height=uint32_t(r.Number(4));const auto time=r.Number(8);Check(!i||height>previous);previous=height;f.branch_mtp.emplace(height,time);}
        e.orchard_replay=std::move(f);
    }
    Check(r.bytes.empty());return e;
}
inline Bytes EventKey(uint64_t sequence){Check(sequence>0);Writer w;w.Number('e',1);w.Number(sequence,8);return std::move(w.bytes);}
} // namespace replay_spool_codec
} // namespace dinero::wallet::detail
