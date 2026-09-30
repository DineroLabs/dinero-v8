#pragma once
#include "daemon/services/assumeutxo_replay.h"
#include "daemon/services/logger_service.h"
#include "daemon/block_acceptor.h"
#include "daemon/config.h"
#include "consensus/block_validation.h"
#include "consensus/block_lifecycle.h"
#include "consensus/state_commitment.h"
#include "consensus/shielded/shielded_block_section.h"
#include "consensus/shielded/nullifier_set.h"
#include "consensus/shielded/anchor_history.h"
#include "consensus/genesis_canonical.h"
#include "consensus/chainwork.h"
#include "storage/block_storage.h"
#include "storage/chain_db.h"
namespace {
// Actual ordinary validation/replay and Init-created owners, in both builds.
// Test-only publication checks the loaded coin inventory against completed replay.
struct PoolPaymentChainFixture {
    dinero::ChainParams previous_params=dinero::Params();
    bool previous_stateless=GetConfig().utreexo_stateless;
    DaemonContext* previous=DaemonContext::instance();
    std::filesystem::path root;
    dinero::ChainDB db;
    dinero::CBlockIndex tip;
    dinero::assumeutxo::AssumeUtxoReplayEngine replay;
    dinero::NullLogger logger;
    DaemonContext context;
    std::shared_ptr<dinero::BlockStorage> files=std::make_shared<dinero::BlockStorage>();
    std::shared_ptr<dinero::ChainstateService> source=std::make_shared<dinero::ChainstateService>();
    std::vector<dinero::Block> blocks;
    const std::vector<uint8_t> coinbase_script;
    const std::vector<uint8_t> funding_script;
    const dinero::ChainWriteToken token=dinero::ChainWriteToken::CreateForTesting();
    static void need(bool ok){if(!ok)throw std::runtime_error("actual pool canonical source fixture");}
    explicit PoolPaymentChainFixture(const std::filesystem::path& directory,std::vector<uint8_t> script={0x51},std::vector<uint8_t> funding={}):root(directory),coinbase_script(std::move(script)),funding_script(std::move(funding)) {
        try { initialize(); } catch (...) { cleanup(); throw; }
    }
    void initialize() {
        using namespace dinero;
        MutableParams().orchard_activation_height=UINT32_MAX;MutableParams().orchard_branch_id=0;
        GetConfig().utreexo_stateless=false;
        need(std::filesystem::create_directory(root));need(db.init(root/"chain")==Status::Ok);need(files->init(root/"archive")==Status::Ok);
        Block genesis;genesis.header=BuildCanonicalGenesis(Params()).header;Transaction initial;
        need(TransactionSerializer::Deserialize(initial,Params().genesis.genesisCoinbaseHex));genesis.vtx.push_back(std::move(initial));blocks.push_back(genesis);
        consensus::ConsensusUTXOSet coins;
        consensus::shielded::CommitmentTree shielded_tree;
        consensus::shielded::NullifierSet shielded_nullifiers;
        consensus::shielded::AnchorHistory shielded_anchors;
        need(shielded_nullifiers.Open(":memory:")==consensus::shielded::NullifierSet::OpenResult::Ok);
        consensus::BlockValidator validator(&coins);
        validator.setShieldedState(&shielded_tree,&shielded_nullifiers,&shielded_anchors);
        for(uint32_t i=0;i<genesis.vtx[0].vout.size();++i) {
            const auto& out=genesis.vtx[0].vout[i];need(coins.AddCoin(OutPoint(genesis.vtx[0].GetTxid(),i),consensus::UTXOEntry(out.value,out.scriptPubKey,0,true,out.is_confidential,out.commitment)));
        }
        std::string error;need(replay.SeedGenesis(genesis,error));
        for(uint32_t h=1;h<=101;++h) {
            Block block;block.header.version=1;block.header.prev_block_hash=blocks.back().GetHash();
            block.header.timestamp=genesis.header.timestamp+h*120;block.header.difficulty=0x1d00ffff;block.header.ZeroReserved();
            Transaction cb;cb.version=2;TxInput in;in.prevout.txid=TxId();in.prevout.vout=UINT32_MAX;in.sequence=UINT32_MAX;
            in.scriptSig={uint8_t(h),uint8_t(h>>8),uint8_t(h>>16),uint8_t(h>>24)};cb.vin.push_back(in);
            // Intentionally claim only20,000una, below the permitted subsidy.
            // This preserves the existing wallet request amount without inventing
            // a reward in the pool table that disagrees with its real coinbase.
            // Optional wallet funding is an actual height-two coinbase output.
            // Height one's pool allocation remains exactly20,000una.
            const bool wallet_funding=h==2 && !funding_script.empty();
            TxOutput out;out.value=AmountUna::Una(wallet_funding?100000:20000);
            out.scriptPubKey=wallet_funding?funding_script:coinbase_script;cb.vout.push_back(out);
            if(consensus::IsStateCommitmentActive(h,Params().state_commitment_activation_height)) {
                // This generated coinbase-only history has no nullifiers. A
                // failed enumeration/count is never treated as empty state.
                const auto count=shielded_nullifiers.TryCount();need(count && *count==0);
                const auto commitment=consensus::shielded::PredictPostBlockShieldedRoot({},h,
                    Params().shielded_epoch_reset_height,Params().shielded_spend_auth_epoch_reset_height,
                    Params().shielded_activation_height,shielded_tree,{},shielded_anchors);
                need(commitment.has_value());
                cb.vout.emplace_back(AmountUna::Zero(),consensus::BuildStateCommitmentScript(*commitment));
            }
            block.vtx.push_back(cb);
            block.header.merkle_root=cb.GetTxid().AsUint256();uint256 root_hash;
            need(validator.ComputeUtreexoRootPure(block,h,root_hash,error));block.header.utreexo_root=root_hash;
            consensus::BlockUndo undo;if(!validator.ConnectBlock(block,h,block.GetHash(),undo,error))throw std::runtime_error("pool fixture validator height "+std::to_string(h)+": "+error);
            if(!replay.ConnectAndAdvance(block,h,block.GetHash(),error))throw std::runtime_error("pool fixture replay height "+std::to_string(h)+": "+error);blocks.push_back(std::move(block));
        }
        arith_uint256 work{0};
        for(uint32_t h=0;h<blocks.size();++h) {
            const auto& b=blocks[h];work+=GetBlockProof(b.header.difficulty);
            need(db.putHeader(token,b.GetHash(),b.header,h,work)==Status::Ok);need(db.putHeightIndex(token,h,b.GetHash())==Status::Ok);
            need(db.putBlock(token,b.GetHash(),b)==Status::Ok);const auto position=files->writeBlock(b.GetHash(),b);need(position.ok());
            ChainDB::PersistedHeaderMetadata m;m.height=h;m.parent_hash=b.header.prev_block_hash;m.chainwork=work;
            m.status_flags=BLOCK_HAVE_DATA|BLOCK_VALID_CHAIN|BLOCK_VALID_SCRIPTS;m.file_number=position->file_number;m.data_pos=position->offset;m.data_size=position->size;
            need(db.putHeaderMetadata(token,b.GetHash(),m)==Status::Ok);
            for(uint32_t i=0;i<b.vtx.size();++i)need(db.putTxIndex(token,b.vtx[i].GetTxid().AsUint256(),b.GetHash(),i)==Status::Ok);
        }
        tip=CBlockIndex(blocks.back().header,101);tip.chainwork=work.GetHex();
        need(db.setTip(token,tip.hash,tip.height,work)==Status::Ok);need(db.setValidatedTip(token,tip.hash,tip.height)==Status::Ok);
        for(const auto& [point,entry]:replay.ProvenUtxos()) {
            Coin c;c.amount=entry.value.GetUna();c.script_pubkey=util::hex(entry.scriptPubKey);c.height=entry.height;c.coinbase=entry.isCoinbase;c.is_confidential=entry.is_confidential;c.commitment=entry.commitment;
            need(db.putCoin(token,point.txid.AsUint256(),point.vout,c)==Status::Ok);
        }
        need(db.putForestTipMarker(token,{101,tip.hash,uint256::FromHexUnsafe(replay.UtreexoRootHex())})==Status::Ok);
        auto cfg=std::make_shared<ConfigService>();cfg->Set("datadir",(root/"runtime").string());context.config=cfg;
        context.logger=std::make_shared<LoggerService>("");context.logger_interface=&logger;context.chainstate=source;context.block_storage=files;
        DaemonContext::setInstance(&context);source->setChainDB(&db);need(source->Init(context));
        WalletBatchPaymentTestAccess::InstallValidatedParent(*source,tip,replay);
        const auto hash=source->getCanonicalBlockHash(1);need(hash.ok() && *hash==blocks[1].GetHash());
    }
    ~PoolPaymentChainFixture() { cleanup(); }
    void cleanup() {
        if(source)source->Stop();context.chainstate.reset();source.reset();files->close();db.close();
        dinero::BlockAcceptor::SetContext(previous);DaemonContext::setInstance(previous);
        dinero::MutableParams()=previous_params;GetConfig().utreexo_stateless=previous_stateless;
        std::error_code ec;std::filesystem::remove_all(root,ec);
    }
};
} // namespace
