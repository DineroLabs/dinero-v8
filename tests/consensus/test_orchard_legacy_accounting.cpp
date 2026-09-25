#include "consensus/orchard_legacy_accounting.h"
#include "consensus/orchard_state_transition.h"
#include "consensus/chainparams.h"
#include "consensus/merkle_root.h"
#include "../storage/shielded_store_fixture.h"
#include <source_location>
using namespace dinero;
using namespace dinero::consensus;
using namespace shielded_store_fixture;
static const auto token = ChainWriteToken::CreateForTesting();
static uint256 H(unsigned char n) { uint256 h; h.begin()[0]=n; return h; }
static Transaction Coinbase(uint32_t height, uint64_t value=1000) {
    Transaction tx; TxInput in; in.prevout.vout=UINT32_MAX;
    in.scriptSig={1,uint8_t(height)};tx.vin={in};
    tx.vout.emplace_back(AmountUna::Una(value),std::vector<uint8_t>{0x51});return tx;
}
// Storage/accounting fixture, not signatures, proof validity or historical
// replay evidence. The bundle is opaque: accounting consumes public flows.
static Transaction Shielded(const std::optional<OutPoint>& input, uint64_t output, uint64_t fee) {
    Transaction tx;tx.version=Transaction::TX_VERSION_SHIELDED_V2;
    tx.shielded_bundle_bytes={1,2,3};tx.SetExplicitFee(fee);
    if(input){TxInput in;in.prevout.txid=input->txid;in.prevout.vout=input->vout;tx.vin={in};}
    tx.vout.emplace_back(AmountUna::Una(output),std::vector<uint8_t>{0x51});return tx;
}
static Block Body(uint32_t height,const uint256& parent,std::vector<Transaction> txs={}) {
    Block b;b.header={};b.header.version=1;b.header.prev_block_hash=parent;b.header.timestamp=100+height;
    b.vtx={Coinbase(height)};b.vtx.insert(b.vtx.end(),txs.begin(),txs.end());
    b.header.merkle_root=ComputeMerkleRoot(b.vtx);return b;
}
static void Store(ChainDB& db,const Block& b,uint32_t height,bool tip=false) {
    auto hash=b.GetHash();
    CHECK(db.putHeader(token,hash,b.header,height,arith_uint256(height))==Status::Ok);
    CHECK(db.putHeightIndex(token,height,hash)==Status::Ok);
    CHECK(db.putBlock(token,hash,b)==Status::Ok);
    for(uint32_t i=0;i<b.vtx.size();++i)
        CHECK(db.putTxIndex(token,b.vtx[i].GetTxid().AsUint256(),hash,i)==Status::Ok);
    if(tip){CHECK(db.setTip(token,hash,height,arith_uint256(height))==Status::Ok);
        CHECK(db.setValidatedTip(token,hash,height)==Status::Ok);}
}
template<class F> static void Reject(Status expected,F fn,
    std::source_location where=std::source_location::current()) {
    bool failed=false;try{fn();}catch(const OrchardStateLookupError& e){
        if(e.SourceStatus()!=expected)std::cerr<<"line "<<where.line()<<": expected "
            <<StatusToString(expected)<<", got "<<StatusToString(e.SourceStatus())<<'\n';
        CHECK(e.SourceStatus()==expected);failed=true;}CHECK(failed);
}
static void Run(bool same_block) {
    struct RestoreParams { ChainParams saved=Params(); ~RestoreParams(){MutableParams()=saved;} } restore_params;
    MutableParams().orchard_activation_height=5;MutableParams().orchard_branch_id=1;
    MutableParams().shielded_activation_height=1;
    MutableParams().shielded_epoch_reset_height=2;
    MutableParams().shielded_cv_binding_activation_height=2;
    MutableParams().shielded_spend_auth_epoch_reset_height=UINT32_MAX;
    uint256 genesis;CHECK(uint256::FromHex(Params().genesis_hash,genesis));
    TempDir temp;Seed(temp.path);ChainDB db;CHECK(db.init(temp.path)==Status::Ok);
    CHECK(db.putHeightIndex(token,0,genesis)==Status::Ok);
    auto funding=Body(1,genesis);Store(db,funding,1);
    auto reset=Body(2,funding.GetHash());Store(db,reset,2);
    auto point=OutPoint(funding.vtx[0].GetTxid(),0);
    std::vector<Transaction> txs;
    if(same_block){
        Transaction intermediate;TxInput in;in.prevout.txid=point.txid;in.prevout.vout=point.vout;intermediate.vin={in};
        intermediate.vout.emplace_back(AmountUna::Una(1000),std::vector<uint8_t>{0x51});
        txs.push_back(intermediate);point=OutPoint(intermediate.GetTxid(),0);
    }
    txs.push_back(Shielded(point,900,10)); // public deposit = 90
    auto deposit=Body(3,reset.GetHash(),txs);Store(db,deposit,3);
    auto withdrawal=Body(4,deposit.GetHash(),{Shielded({},20,2)});Store(db,withdrawal,4,true);
    const auto derive=[&](uint32_t budget=3){return DeriveSelectedLegacyPoolAccountingUnderLock(db,nullptr,budget);};
    auto result=derive();CHECK(result.value_una==68 && result.epoch_height==2);
    CHECK(result.parent_height==4 && result.parent_hash==withdrawal.GetHash());
    CHECK(result.blocks_read==3 && result.shielded_transactions==2);
    db.close();auto before=Inspect(temp.path);CHECK(db.init(temp.path)==Status::Ok);
    CHECK(derive().value_una==68);db.close();CHECK(Inspect(temp.path)==before);CHECK(db.init(temp.path)==Status::Ok);
    Reject(Status::Invalid,[&]{(void)derive(2);});Reject(Status::Invalid,[&]{(void)derive(0);});
    // Missing or corrupt bodies, source index and ancestry are not zero value.
    CHECK(db.deleteBlock(token,deposit.GetHash())==Status::Ok);
    Reject(Status::NotFound,[&]{(void)derive();});Store(db,deposit,3);
    // The changed coinbase is not a referenced prevout. Only the whole-body
    // Merkle check protects this case; an origin txid check cannot mask it.
    auto bad_body=deposit;bad_body.vtx[0].vout[0].value=AmountUna::Una(999);
    CHECK(db.putBlock(token,deposit.GetHash(),bad_body)==Status::Ok);
    Reject(Status::Corruption,[&]{(void)derive();});Store(db,deposit,3);
    auto bad=funding;bad.vtx[0].vout[0].value=AmountUna::Una(999);
    if(!same_block){
        CHECK(db.putBlock(token,funding.GetHash(),bad)==Status::Ok);
        Reject(Status::Corruption,[&]{(void)derive();});Store(db,funding,1);
    }
    CHECK(db.putTxIndex(token,point.txid.AsUint256(),withdrawal.GetHash(),1)==Status::Ok);
    Reject(Status::Corruption,[&]{(void)derive();});
    if(same_block)Store(db,deposit,3);else Store(db,funding,1);
    CHECK(db.putHeightIndex(token,2,H(99))==Status::Ok);
    Reject(Status::NotFound,[&]{(void)derive();});Store(db,reset,2);
    CHECK(db.setValidatedTip(token,deposit.GetHash(),3)==Status::Ok);
    Reject(Status::Corruption,[&]{(void)derive();});Store(db,withdrawal,4,true);
    // Complete competing selected suffix recalculates rather than reusing an amount.
    auto fork=Body(4,deposit.GetHash(),{Shielded({},30,3)});Store(db,fork,4,true);
    CHECK(derive().value_una==57 && derive().parent_hash==fork.GetHash());
    auto missing_fee=Shielded({},20,2);missing_fee.has_explicit_fee=false;
    Store(db,Body(4,deposit.GetHash(),{missing_fee}),4,true);
    Reject(Status::Invalid,[&]{(void)derive();});
    auto legacy=Shielded({},20,2);legacy.version=Transaction::TX_VERSION_SHIELDED;
    Store(db,Body(4,deposit.GetHash(),{legacy}),4,true);
    Reject(Status::Invalid,[&]{(void)derive();});
    Store(db,Body(4,deposit.GetHash(),{Shielded({},91,0)}),4,true);
    Reject(Status::Corruption,[&]{(void)derive();});
    Store(db,Body(4,deposit.GetHash(),{Shielded({},0,orchard::kMaxMoneyUna+1)}),4,true);
    Reject(Status::Corruption,[&]{(void)derive();});
    auto duplicate=Shielded(point,900,10);duplicate.vin.push_back(duplicate.vin.front());
    Store(db,Body(4,deposit.GetHash(),{duplicate}),4,true);
    Reject(Status::Corruption,[&]{(void)derive();});
    auto hidden=Shielded({},20,2);hidden.vout[0].is_confidential=true;
    hidden.vout[0].commitment=std::vector<uint8_t>(33,2);
    hidden.vout[0].nonce=std::vector<uint8_t>(65,2);hidden.vout[0].range_proof={1};
    Store(db,Body(4,deposit.GetHash(),{hidden}),4,true);
    Reject(Status::Invalid,[&]{(void)derive();});
    Store(db,withdrawal,4,true);
    MutableParams().orchard_activation_height=UINT32_MAX;MutableParams().orchard_branch_id=0;
    Reject(Status::Invalid,[&]{(void)derive();});
    MutableParams().orchard_activation_height=5;MutableParams().orchard_branch_id=1;
    // A later reset discards the earlier epoch; activity in the reset block is
    // inconsistent historical input, not an amount to carry across the reset.
    MutableParams().shielded_spend_auth_epoch_reset_height=4;
    Reject(Status::Corruption,[&]{(void)derive();});
    auto empty=Body(4,deposit.GetHash());Store(db,empty,4,true);
    result=derive(1);CHECK(result.epoch_height==4 && result.value_una==0 && result.blocks_read==1);
    // Both reset parameters dormant: begin at initial pool activation instead.
    MutableParams().shielded_epoch_reset_height=UINT32_MAX;
    MutableParams().shielded_spend_auth_epoch_reset_height=UINT32_MAX;
    result=derive(4);CHECK(result.epoch_height==1 && result.value_una==90 && result.blocks_read==4);
    CHECK(db.putHeightIndex(token,0,H(99))==Status::Ok);
    Reject(Status::Corruption,[&]{(void)derive(4);});
    CHECK(db.putHeightIndex(token,0,genesis)==Status::Ok);
    db.close();Reject(Status::Internal,[&]{(void)derive();});
}
int main(){try{SelectParams(Chain::REGTEST);Run(false);Run(true);
    std::cout<<"PASS selected legacy public-flow accounting, reset, same-block origins, reorg and unavailable sources\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
