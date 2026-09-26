#include "orchard_block_test_fixture.h"
#include "wallet/orchard_account_state.h"
#include "daemon/runtime_block_outbox.h"
#include "primitives/block.h"
#ifdef DINERO_TEST_BOUND_ACCOUNT
#include "wallet/orchard_account_delivery.h"
#include "wallet/wallet_manager.h"
#include "consensus/chainparams.h"
#endif
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#include <filesystem>
#include <sqlite3.h>
#include <unistd.h>
using dinero::wallet::OrchardAccountState;
template <class F> static void AccountReject(F fn) {
  bool failed = false;
  try {
    fn();
  } catch (const std::exception &) {
    failed = true;
  }
  Require(failed);
}
int main(int argc, char **argv) {
  try {
    Require(argc == 2 || argc == 5);
    Fixture f(argv[1]);
    if (argc == 5) {
      // Fresh process: exit without SQLite/C++ cleanup at either boundary.
      sqlite3 *source = nullptr, *target = nullptr;
      Require(sqlite3_open(argv[2], &source) == SQLITE_OK);
      WalletStorageIdentity identity{WalletNetwork::Regtest,
                                     f.domain.genesis_wire, Hash{44}, 0};
      const std::array<uint8_t, 64> seed{7};
      WalletStateBytes next = [&] {
        WalletSnapshotStore store(source, identity, seed);
        return std::move(store.Read()->state);
      }();
      Require(sqlite3_close(source) == SQLITE_OK);
      Require(sqlite3_open(argv[3], &target) == SQLITE_OK);
      Require(sqlite3_exec(target, "PRAGMA synchronous=FULL;BEGIN IMMEDIATE;",
                           nullptr, nullptr, nullptr) == SQLITE_OK);
      WalletSnapshotStore store(target, identity, seed);
      const auto prior = store.Read();
      Require(prior.has_value());
      Require(store.StageReplace(prior->revision, next) == prior->revision + 1);
      if (std::string(argv[4]) == "after")
        Require(sqlite3_exec(target, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK);
      else Require(std::string(argv[4]) == "before");
      _exit(0);
    }
    auto keys = WalletKeys::FromSeed(std::array<uint8_t, 64>{7}, 0);
    const auto fvk = keys.ExportFullViewingKey();
    auto initial = OrchardAccountState::Begin(f.domain, fvk, 20001, H(1));
    auto [first, r0] = initial.IssueReceiver(WalletScope::External);
    auto [second, r1] = first.IssueReceiver(WalletScope::External);
    auto [issued, internal] = second.IssueReceiver(WalletScope::Internal);
    Require(r0 != r1 && internal != r0 &&
            r0 == keys.Receiver(WalletScope::External, {}));
    DiversifierIndex one{};
    one[0] = 1;
    Require(r1 == keys.Receiver(WalletScope::External, one));
    f.outputs[0].script_pub_key =
        f.view.coins.at(Point(f.inputs[0])).scriptPubKey;
    f.outputs[1].script_pub_key =
        f.view.coins.at(Point(f.inputs[1])).scriptPubKey;
    f.outputs[1].amount_una = 51000;
    std::vector<ResolvedInput> inputs;
    for (const auto &input : f.inputs) {
      const auto &coin = f.view.coins.at(Point(input));
      inputs.push_back({input.txid_wire, input.output_index, input.sequence,
                        coin.value.GetUna(), coin.scriptPubKey});
    }
    auto context =
        SigningContext::Create(f.domain, f.lock, inputs, f.outputs, f.fee);
    std::vector<WalletPayment> payments{{5000, r0}};
    auto plan = WalletBundlePlan::PrepareShield(keys, payments);
    auto intent = plan.Intent(context);
    const Hash id{42};
    auto reserved = issued.Reserve(id, intent);
    auto proof = std::move(plan).Prove(context);
    f.bundle = proof.Bytes();
    f.Sign();
    std::vector<VerifiedOrchardAuthorizations> auths{
        VerifyOrchardAuthorizations(f.Snapshot(), f.domain, 20001, {})};
    auto ready = reserved.SetReady(id, auths[0]);
    OrchardBlockContext c{20001, H(2), H(1), 20001, f.domain};
    auto block = Candidate(c, auths);
    c.block_hash = block.Header().GetHash();
    OrchardStateLookups lookups{
        [](const uint256 &) -> StatusOr<bool> { return false; },
        [](const uint256 &) -> StatusOr<bool> { return false; }};
    auto transition =
        PrepareOrchardStateTransition(c, std::nullopt, auths, lookups);
    auto funded = ready.Advance(c, block, transition, auths);
    Require(funded.Scan().BalanceUna() == 5000 &&
            funded.Operations().Entries().at(id).transaction ==
                f.Build().CanonicalBytes());
    Require(funded.Observations().at(id).outcome ==
            OrchardAccountState::OperationOutcome::Confirmed);
    Require(funded.Observations().at(id).block_hash == c.block_hash);
    // Ordered source receipts are inseparable from applying real note effects.
    // Synthetic receipt digests here are not evidence of source-log validation.
    dinero::RuntimeOutboxEvent received{{1, H(71)}, {},
        dinero::RuntimeBlockDirection::Connect, c, block.WireBytes()};
    auto delivered = ready.AdvanceDelivery(received, block, transition, auths);
    Require(delivered.Scan().BalanceUna() == 5000 &&
            delivered.Delivery().sequence == 1 && delivered.Delivery().digest == H(71));
    Require(delivered.Observations() == funded.Observations());
    auto removed = received;
    removed.direction = dinero::RuntimeBlockDirection::Disconnect;
    removed.previous_digest = H(71);
    removed.cursor = {2, H(72)};
    auto rolled = delivered.RewindDelivery(removed, block, initial);
    Require(rolled.Scan().BalanceUna() == 0 && rolled.Delivery().sequence == 2 &&
            rolled.Delivery().digest == H(72) && rolled.Observations().empty());
    Require(rolled.Operations().Entries().at(id).transaction ==
            ready.Operations().Entries().at(id).transaction);
    auto again = received;
    again.previous_digest = H(72);
    again.cursor = {3, H(73)};
    auto redelivered = rolled.AdvanceDelivery(again, block, transition, auths);
    Require(redelivered.Scan().Checkpoint() == delivered.Scan().Checkpoint() &&
            redelivered.Delivery().sequence == 3 && redelivered.Delivery().digest == H(73));
    AccountReject([&] { (void)delivered.AdvanceDelivery(received, block, transition, auths); });
    AccountReject([&] { (void)redelivered.RewindDelivery(removed, block, initial); });
    AccountReject([&] { (void)delivered.RewindScanFrom(initial); });
    AccountReject([&] { (void)rolled.Advance(c, block, transition, auths); });
    AccountReject([&] { (void)delivered.RewindDelivery(removed, block, delivered); });
    const auto rejectDelivery = [&](auto mutate) {
      auto bad = received;
      mutate(bad);
      AccountReject([&] { (void)ready.AdvanceDelivery(bad, block, transition, auths); });
    };
    rejectDelivery([](auto &e) { e.cursor.sequence = 2; });
    rejectDelivery([](auto &e) { e.cursor.digest = {}; });
    rejectDelivery([](auto &e) { e.previous_digest = H(9); });
    rejectDelivery([](auto &e) { e.direction = dinero::RuntimeBlockDirection::Disconnect; });
    rejectDelivery([](auto &e) { e.context.domain.branch_id++; });
    rejectDelivery([](auto &e) { e.context.activation_height++; });
    rejectDelivery([](auto &e) { e.context.height++; });
    rejectDelivery([](auto &e) { e.context.parent_hash = H(9); });
    rejectDelivery([](auto &e) { e.context.block_hash = H(9); });
    rejectDelivery([](auto &e) { e.body.back() ^= 1; });
    auto rewound = funded.RewindScanFrom(initial);
    Require(rewound.Observations().empty());
    Require(rewound.Scan().BalanceUna() == 0 &&
            rewound.Operations().Entries().size() == 1);
    DiversifierIndex two{};
    two[0] = 2;
    auto [afterRewind, r2] = rewound.IssueReceiver(WalletScope::External);
    Require(r2 == keys.Receiver(WalletScope::External, two));
    auto [afterInternal, ri1] = rewound.IssueReceiver(WalletScope::Internal);
    Require(ri1 == keys.Receiver(WalletScope::Internal, one));
    auto replacement = Candidate(c, auths, {}, 17);
    auto alt = c;
    alt.block_hash = replacement.Header().GetHash();
    auto altTransition =
        PrepareOrchardStateTransition(alt, std::nullopt, auths, lookups);
    auto replaced = rewound.Advance(alt, replacement, altTransition, auths);
    Require(replaced.Scan().BalanceUna() == 5000 &&
            replaced.Scan().Checkpoint().block_hash !=
                funded.Scan().Checkpoint().block_hash);
    dinero::wallet::OrchardWalletRestoreLookups restoreLookups{
        [&](uint32_t height, const uint256 &blockHash, const Hash &txid) {
          Require(height == c.height && blockHash == c.block_hash &&
                  txid == auths[0].Orchard().Txid());
          return std::make_shared<const VerifiedOrchardAuthorizations>(
              auths[0]);
        },
        [](const uint256 &) -> StatusOr<bool> { return false; }};
    restoreLookups.selected_block = [&](uint32_t height, const uint256 &hash) {
      Require(height == c.height && hash == c.block_hash);
      return std::make_shared<const OrchardBlockCandidate>(block);
    };
    const auto HistoryRequire=[](bool ok,const char* what) {if(!ok)throw std::runtime_error(what);};
    // Real historical bodies around the activation parent; these are body and
    // wallet-effect fixtures, not independently validated historical consensus.
    Block historyParent; historyParent.header={};historyParent.header.version=1;
    historyParent.header.prev_block_hash=H(90);historyParent.header.timestamp=20000;
    Transaction historicalCoinbase;historicalCoinbase.version=2;
    TxInput historyCb;historyCb.prevout.vout=UINT32_MAX;historyCb.scriptSig={2,32,78};
    historicalCoinbase.vin={historyCb};historicalCoinbase.vout.emplace_back(AmountUna::Una(1),Bytes{0x51});
    historyParent.vtx={historicalCoinbase};historyParent.header.merkle_root=ComputeMerkleRoot(historyParent.vtx);
    auto historyInitial=OrchardAccountState::RestoreForRescan(ready.Encode(),f.domain,fvk,20001,historyParent.GetHash());
    auto historyContext=c;historyContext.parent_hash=historyParent.GetHash();
    auto historyBoundary=Candidate(historyContext,auths);historyContext.block_hash=historyBoundary.Header().GetHash();
    auto historyTransition=PrepareOrchardStateTransition(historyContext,std::nullopt,auths,lookups);
    auto historicalReceipt=[&](const Block& b,dinero::RuntimeBlockDirection direction,uint64_t sequence,uint8_t digest,uint8_t previous) {
      auto ec=historyContext;ec.height=20000;ec.block_hash=b.GetHash();ec.parent_hash=b.header.prev_block_hash;
      const auto wire=b.Serialize();
      return dinero::RuntimeOutboxEvent{{sequence,H(digest)},previous?H(previous):uint256{},direction,ec,Bytes(wire.begin(),wire.end())};
    };
    dinero::RuntimeOutboxEvent historyUp{{1,H(101)},{},dinero::RuntimeBlockDirection::Connect,historyContext,historyBoundary.WireBytes()};
    auto historyFunded=historyInitial.AdvanceDelivery(historyUp,historyBoundary,historyTransition,auths);
    auto historyRemove=historyUp;historyRemove.direction=dinero::RuntimeBlockDirection::Disconnect;
    historyRemove.cursor={2,H(102)};historyRemove.previous_digest=H(101);
    auto historyEmpty=historyFunded.RewindDelivery(historyRemove,historyBoundary,historyInitial);
    auto historyDown=historicalReceipt(historyParent,dinero::RuntimeBlockDirection::Disconnect,3,103,102);
    auto below=historyEmpty.ApplyHistoricalDelivery(historyDown);
    HistoryRequire(below.Scan().Checkpoint().height==19999 && below.Scan().Checkpoint().block_hash==H(90),"historical applied tip");
    Require(below.Scan().Notes().empty() && below.Scan().BalanceUna()==0 && below.Observations().empty());
    auto historyAlternate=historyParent;historyAlternate.header.nonce=17;
    Transaction historicalConflict;historicalConflict.version=2;
    TxInput conflictInput;conflictInput.prevout.txid=Point(f.inputs[0]).txid;conflictInput.prevout.vout=Point(f.inputs[0]).vout;historicalConflict.vin={conflictInput};
    historicalConflict.vout.emplace_back(AmountUna::Una(1),Bytes{0x51});
    historyAlternate.vtx.push_back(historicalConflict);historyAlternate.header.merkle_root=ComputeMerkleRoot(historyAlternate.vtx);
    auto historyConnect=historicalReceipt(historyAlternate,dinero::RuntimeBlockDirection::Connect,4,104,103);
    auto historicalConflicted=below.ApplyHistoricalDelivery(historyConnect);
    HistoryRequire(historicalConflicted.Observations().contains(id),"historical conflict retained");
    Require(historicalConflicted.Observations().at(id).outcome==OrchardAccountState::OperationOutcome::Conflicted);
    Require(historicalConflicted.Observations().at(id).height==20000);
    const auto historicalConflictTxid=historicalConflict.GetTxid().AsUint256();
    Hash historicalConflictId{};std::copy(historicalConflictTxid.begin(),historicalConflictTxid.end(),historicalConflictId.begin());
    Require(historicalConflicted.Observations().at(id).transaction_id==historicalConflictId);
    Require(historicalConflicted.Delivery().sequence==4 && historicalConflicted.Scan().Checkpoint().block_hash==historyAlternate.GetHash());
    dinero::wallet::OrchardWalletRestoreLookups historicalLookups;
    historicalLookups.selected_historical_block=[&](uint32_t height,const uint256& hash) {
      Require(height==20000 && hash==historyAlternate.GetHash());
      return std::make_shared<const Block>(historyAlternate);
    };
    auto restoredHistorical=OrchardAccountState::Restore(historicalConflicted.Encode(),f.domain,fvk,20001,
        historicalConflicted.Scan().Checkpoint(),historicalLookups);
    Require(restoredHistorical.Observations()==historicalConflicted.Observations());
    Require(restoredHistorical.Operations().Entries().at(id).transaction==ready.Operations().Entries().at(id).transaction);
    Require(restoredHistorical.IssueReceiver(WalletScope::External).second==historyInitial.IssueReceiver(WalletScope::External).second);
    AccountReject([&]{(void)OrchardAccountState::Restore(historicalConflicted.Encode(),f.domain,fvk,20001,historicalConflicted.Scan().Checkpoint(),{});});
    auto wrongHistorical=historicalLookups;wrongHistorical.selected_historical_block=[&](uint32_t,const uint256&) {return std::make_shared<const Block>(historyParent);};
    AccountReject([&]{(void)OrchardAccountState::Restore(historicalConflicted.Encode(),f.domain,fvk,20001,historicalConflicted.Scan().Checkpoint(),wrongHistorical);});
    auto oldHistoricalBytes=historicalConflicted.Encode();auto oldHistorical=Bytes(oldHistoricalBytes.Bytes().begin(),oldHistoricalBytes.Bytes().end());oldHistorical[7]='4';
    AccountReject([&]{(void)OrchardAccountState::RestoreForRescan(WalletStateBytes(oldHistorical),f.domain,fvk,20001,historyParent.GetHash());});
    auto undoHistory=historicalReceipt(historyAlternate,dinero::RuntimeBlockDirection::Disconnect,5,105,104);
    auto historicalUndone=historicalConflicted.ApplyHistoricalDelivery(undoHistory);
    HistoryRequire(historicalUndone.Observations().empty(),"historical conflict reverted");
    Require(historicalUndone.Scan().Checkpoint()==below.Scan().Checkpoint());
    auto backHistory=historicalReceipt(historyParent,dinero::RuntimeBlockDirection::Connect,6,106,105);
    auto historyBack=historicalUndone.ApplyHistoricalDelivery(backHistory);
    auto historyAgain=historyUp;historyAgain.cursor={7,H(107)};historyAgain.previous_digest=H(106);
    auto historyRefilled=historyBack.AdvanceDelivery(historyAgain,historyBoundary,historyTransition,auths);
    Require(historyRefilled.Scan().BalanceUna()==5000 && historyRefilled.Observations().at(id).outcome==OrchardAccountState::OperationOutcome::Confirmed);
    Require(historyRefilled.Operations().Entries().at(id).transaction==ready.Operations().Entries().at(id).transaction);
    Require(historyRefilled.Archive()==historyInitial.Archive());
    AccountReject([&]{(void)historyFunded.ApplyHistoricalDelivery(historyDown);});
    AccountReject([&]{(void)below.AdvanceDelivery(historyAgain,historyBoundary,historyTransition,auths);});
    const auto rejectHistorical=[&](auto change) {auto bad=historyConnect;change(bad);AccountReject([&]{(void)below.ApplyHistoricalDelivery(bad);});};
    rejectHistorical([](auto& e){++e.cursor.sequence;});
    rejectHistorical([](auto& e){e.previous_digest=H(8);});
    rejectHistorical([](auto& e){++e.context.domain.branch_id;});
    rejectHistorical([](auto& e){++e.context.activation_height;});
    rejectHistorical([](auto& e){--e.context.height;});
    rejectHistorical([](auto& e){e.context.parent_hash=H(8);});
    rejectHistorical([](auto& e){e.body.back()^=1;});
    auto badMerkle=historyAlternate;badMerkle.vtx.back().vout[0].value=AmountUna::Una(2);
    bool badMerkleRefused=false;
    try {auto e=historyConnect;const auto wire=badMerkle.Serialize();e.body=Bytes(wire.begin(),wire.end());(void)below.ApplyHistoricalDelivery(e);}
    catch(const std::exception&){badMerkleRefused=true;}
    HistoryRequire(badMerkleRefused,"historical Merkle refusal");
    auto v4bytes=delivered.Encode();auto v4=Bytes(v4bytes.Bytes().begin(),v4bytes.Bytes().end());v4[7]='4';
    Require(OrchardAccountState::Restore(WalletStateBytes(v4),f.domain,fvk,20001,delivered.Scan().Checkpoint(),restoreLookups).Delivery()==delivered.Delivery());

    auto bytes = funded.Encode();
    auto restored = OrchardAccountState::Restore(bytes, f.domain, fvk, 20001,
                                                 funded.Scan().Checkpoint(),
                                                 restoreLookups);
    Require(restored.Scan().BalanceUna() == 5000 &&
            restored.Operations().Entries().at(id).transaction ==
                f.Build().CanonicalBytes());
    Require(restored.Observations() == funded.Observations());
    Require(restored.IssueReceiver(WalletScope::External).second == r2);
    auto receiptRestored = OrchardAccountState::Restore(
        delivered.Encode(), f.domain, fvk, 20001, delivered.Scan().Checkpoint(), restoreLookups);
    Require(receiptRestored.Delivery() == delivered.Delivery() &&
            receiptRestored.Scan().BalanceUna() == 5000);
    Require(delivered.IssueReceiver(WalletScope::External).first.Delivery() == delivered.Delivery());
    auto restartedScan = OrchardAccountState::RestoreForRescan(
        delivered.Encode(), f.domain, fvk, 20001, H(1));
    Require(restartedScan.Delivery().sequence == 0 && restartedScan.Delivery().digest.IsNull() &&
            restartedScan.Scan().BalanceUna() == 0);
    Require(restartedScan.Operations().Entries().at(id).transaction ==
            delivered.Operations().Entries().at(id).transaction);
    auto rescan = OrchardAccountState::RestoreForRescan(bytes, f.domain, fvk,
                                                        20001, H(1));
    Require(rescan.Scan().BalanceUna() == 0 &&
            rescan.Operations().Entries().at(id).transaction ==
                f.Build().CanonicalBytes());
    Require(rescan.Observations().empty());
    Require(rescan.IssueReceiver(WalletScope::External).second == r2);
    Require(rescan.Advance(c, block, transition, auths).Scan().BalanceUna() ==
            5000);
    // An ordinary selected transaction can conflict with the transparent
    // funding reservation. Its script validity is a caller precondition here.
    Transaction competing;
    competing.version = 2;
    TxInput input;
    input.prevout.txid = Point(f.inputs[0]).txid;
    input.prevout.vout = f.inputs[0].output_index;
    competing.vin = {input};
    competing.vout.emplace_back(AmountUna::Una(12000),
                                f.outputs[0].script_pub_key);
    auto transparentBlock = Candidate(c, {}, {competing}, 29);
    auto tc = c;
    tc.block_hash = transparentBlock.Header().GetHash();
    auto transparentTransition =
        PrepareOrchardStateTransition(tc, std::nullopt, {}, lookups);
    auto conflicted =
        ready.Advance(tc, transparentBlock, transparentTransition, {});
    Require(conflicted.Observations().at(id).outcome ==
            OrchardAccountState::OperationOutcome::Conflicted);
    Require(conflicted.Operations().Entries().at(id).transaction ==
            ready.Operations().Entries().at(id).transaction);
    Require(conflicted.RewindScanFrom(initial).Observations().empty());
    // Same-height branch replacement must first rewind to the common ancestor.
    AccountReject([&] { (void)funded.RewindScanFrom(replaced); });

    // Two honest wallet plans spending the same note: selected-chain conflict
    // is detected by nullifier with no transparent inputs in either plan.
    const Hash spendId{43};
    const auto &owned = funded.Scan().Notes()[0];
    std::vector<WalletSpendInput> spendInputs{{*owned.note, *owned.witness}};
    Hash anchor;
    std::copy(std::begin(owned.witness->Facts().root),
              std::end(owned.witness->Facts().root), anchor.begin());
    const std::vector<TransparentOutput> cash{
        {4500, f.outputs[0].script_pub_key}};
    const auto spendContext =
        SigningContext::Create(f.domain, 0, {}, cash, 500);
    auto planA = WalletBundlePlan::PrepareSpend(keys, spendInputs, anchor, {});
    auto pendingSpend = funded.Reserve(spendId, planA.Intent(spendContext));
    auto proofB = WalletBundlePlan::PrepareSpend(keys, spendInputs, anchor, {})
                      .Prove(spendContext);
    const auto txB =
        TransactionEnvelope::Create(0, {}, cash, 500, proofB.Bytes());
    auto spendView = f.view;
    spendView.height = 20001;
    const auto authB = VerifyOrchardAuthorizations(
        OrchardCoinSnapshot::ResolveUnderChainstateLock(txB, spendView),
        f.domain, 20002, {});
    const std::vector<VerifiedOrchardAuthorizations> spendAuthsB{authB};
    auto sc = c;
    sc.height = 20002;
    sc.parent_hash = c.block_hash;
    auto blockB = Candidate(sc, spendAuthsB);
    sc.block_hash = blockB.Header().GetHash();
    auto spendLookups = lookups;
    spendLookups.active_anchor = [](const uint256 &) -> StatusOr<bool> {
      return true;
    };
    auto transitionB = PrepareOrchardStateTransition(sc, transition.Next(),
                                                     spendAuthsB, spendLookups);
    auto spentOther =
        pendingSpend.Advance(sc, blockB, transitionB, spendAuthsB);
    Require(spentOther.Scan().BalanceUna() == 0 &&
            spentOther.Observations().at(spendId).outcome ==
                OrchardAccountState::OperationOutcome::Conflicted);
    const auto proofA = std::move(planA).Prove(spendContext);
    const auto txA =
        TransactionEnvelope::Create(0, {}, cash, 500, proofA.Bytes());
    const auto authA = VerifyOrchardAuthorizations(
        OrchardCoinSnapshot::ResolveUnderChainstateLock(txA, spendView),
        f.domain, 20002, {});
    AccountReject([&] { (void)spentOther.SetReady(spendId, authA); });
    auto resurrected = spentOther.RewindScanFrom(funded);
    Require(!resurrected.Observations().contains(spendId) &&
            resurrected.Observations().contains(id));
    Require(resurrected.Operations().Entries().contains(spendId));
    auto readySpend = resurrected.SetReady(spendId, authA);
    const std::vector<VerifiedOrchardAuthorizations> spendAuthsA{authA};
    auto blockA = Candidate(sc, spendAuthsA, {}, 33);
    auto ac = sc;
    ac.block_hash = blockA.Header().GetHash();
    auto transitionA = PrepareOrchardStateTransition(ac, transition.Next(),
                                                     spendAuthsA, spendLookups);
    auto confirmedSpend =
        readySpend.Advance(ac, blockA, transitionA, spendAuthsA);
    Require(confirmedSpend.Observations().at(spendId).outcome ==
            OrchardAccountState::OperationOutcome::Confirmed);
    Require(confirmedSpend.RewindScanFrom(funded)
                .Operations()
                .Entries()
                .at(spendId)
                .transaction == txA.CanonicalBytes());
    auto historyLookups = restoreLookups;
    historyLookups.selected_block = [&](uint32_t height, const uint256 &hash) {
      if (height == c.height) {
        Require(hash == c.block_hash);
        return std::make_shared<const OrchardBlockCandidate>(block);
      }
      Require(height == sc.height && hash == sc.block_hash);
      return std::make_shared<const OrchardBlockCandidate>(blockB);
    };
    auto conflictRestart = OrchardAccountState::Restore(
        spentOther.Encode(), f.domain, fvk, 20001,
        spentOther.Scan().Checkpoint(), historyLookups);
    Require(conflictRestart.Observations() == spentOther.Observations());
    auto missingHistory = historyLookups;
    missingHistory.selected_block = {};
    AccountReject([&] {
      (void)OrchardAccountState::Restore(spentOther.Encode(), f.domain, fvk,
                                         20001, spentOther.Scan().Checkpoint(),
                                         missingHistory);
    });
    auto wrongHistory = historyLookups;
    wrongHistory.selected_block = [&](uint32_t, const uint256 &) {
      return std::make_shared<const OrchardBlockCandidate>(blockA);
    };
    AccountReject([&] {
      (void)OrchardAccountState::Restore(spentOther.Encode(), f.domain, fvk,
                                         20001, spentOther.Scan().Checkpoint(),
                                         wrongHistory);
    });
    // Old staged account snapshots have no observation section. Decode them
    // without fabricating confirmations; subsequent selected replay fills it.
    auto oldBytes = initial.Encode();
    std::vector<uint8_t> old(oldBytes.Bytes().begin(), oldBytes.Bytes().end());
    old[7] = '1';
    old.resize(old.size() - 84);
    Require(OrchardAccountState::RestoreForRescan(WalletStateBytes(old),
                                                  f.domain, fvk, 20001, H(1))
                .Observations()
                .empty());
    auto prior =
        std::vector<uint8_t>(bytes.Bytes().begin(), bytes.Bytes().end());
    prior[7] = '2';
    prior.resize(prior.size() - 80);
    auto priorRestored = OrchardAccountState::Restore(
        WalletStateBytes(prior), f.domain, fvk, 20001,
        funded.Scan().Checkpoint(), restoreLookups);
    Require(priorRestored.Observations() == funded.Observations() &&
            priorRestored.Archive().count == 0);
    auto v3 = std::vector<uint8_t>(bytes.Bytes().begin(), bytes.Bytes().end());
    v3[7] = '3'; v3.resize(v3.size() - 40);
    auto v3Restored = OrchardAccountState::Restore(WalletStateBytes(v3), f.domain,
        fvk, 20001, funded.Scan().Checkpoint(), restoreLookups);
    Require(v3Restored.Delivery().sequence == 0 && v3Restored.Observations() == funded.Observations());
    auto malformed = delivered.Encode();
    auto badDelivery = std::vector<uint8_t>(malformed.Bytes().begin(), malformed.Bytes().end());
    std::fill(badDelivery.end() - 32, badDelivery.end(), 0);
    AccountReject([&] { (void)OrchardAccountState::RestoreForRescan(
        WalletStateBytes(badDelivery), f.domain, fvk, 20001, H(1)); });
    auto badReceipt =
        std::vector<uint8_t>(bytes.Bytes().begin(), bytes.Bytes().end());
    const auto receiptStart = badReceipt.size() - 181;
    badReceipt[receiptStart + 32] =
        0; // Unknown outcome, not a missing observation.
    AccountReject([&] {
      (void)OrchardAccountState::Restore(WalletStateBytes(badReceipt), f.domain,
                                         fvk, 20001, funded.Scan().Checkpoint(),
                                         restoreLookups);
    });
    auto wrong = keys.ExportFullViewingKey();
    wrong[0] ^= 1;
    AccountReject([&] {
      (void)OrchardAccountState::RestoreForRescan(bytes, f.domain, wrong, 20001,
                                                  H(1));
    });
    auto wrongDomain = f.domain;
    wrongDomain.network_code = 1;
    AccountReject([&] {
      (void)OrchardAccountState::RestoreForRescan(bytes, wrongDomain, fvk,
                                                  20001, H(1));
    });
    AccountReject([&] {
      (void)OrchardAccountState::Restore(bytes, f.domain, fvk, 20001,
                                         replaced.Scan().Checkpoint(),
                                         restoreLookups);
    });
    // Counter boundaries using synthetic authenticated payloads. The final
    // 88-bit receiver can be issued once; exhaustion survives snapshot restore.
    auto zero = initial.Encode();
    auto edge = std::vector<uint8_t>(zero.Bytes().begin(), zero.Bytes().end());
    constexpr size_t externalIndex = 8 + 1 + 32 + 4 + 4 + 32;
    std::fill(edge.begin() + externalIndex, edge.begin() + externalIndex + 11,
              255);
    auto last = OrchardAccountState::RestoreForRescan(
        WalletStateBytes(edge), f.domain, fvk, 20001, H(1));
    auto [exhausted, lastReceiver] = last.IssueReceiver(WalletScope::External);
    DiversifierIndex max;
    max.fill(255);
    Require(lastReceiver == keys.Receiver(WalletScope::External, max));
    AccountReject(
        [&] { (void)exhausted.IssueReceiver(WalletScope::External); });
    auto exhaustedAgain = OrchardAccountState::RestoreForRescan(
        exhausted.Encode(), f.domain, fvk, 20001, H(1));
    AccountReject(
        [&] { (void)exhaustedAgain.IssueReceiver(WalletScope::External); });
    Require(exhaustedAgain.IssueReceiver(WalletScope::Internal).second ==
            internal);
    edge[externalIndex + 11] = 2;
    AccountReject([&] {
      (void)OrchardAccountState::RestoreForRescan(WalletStateBytes(edge),
                                                  f.domain, fvk, 20001, H(1));
    });
    for (size_t n = 0; n < zero.Bytes().size(); ++n)
      AccountReject([&] {
        (void)OrchardAccountState::RestoreForRescan(
            WalletStateBytes(zero.Bytes().first(n)), f.domain, fvk, 20001,
            H(1));
      });
    auto extra = std::vector<uint8_t>(zero.Bytes().begin(), zero.Bytes().end());
    extra.push_back(0);
    AccountReject([&] {
      (void)OrchardAccountState::RestoreForRescan(WalletStateBytes(extra),
                                                  f.domain, fvk, 20001, H(1));
    });
    // One ciphertext contains the note/witness, counters and ready transaction.
    auto pattern =
        (std::filesystem::temp_directory_path() / "orchard-account-XXXXXX")
            .string();
    std::vector<char> dir(pattern.begin(), pattern.end());
    dir.push_back(0);
    Require(mkdtemp(dir.data()));
    struct Cleanup {
      std::filesystem::path p;
      ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{dir.data()};
    auto path = (cleanup.p / "wallet.sqlite").string();
    sqlite3 *db = nullptr;
    Require(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    auto sql = [&](const char *q) {
      Require(sqlite3_exec(db, q, nullptr, nullptr, nullptr) == SQLITE_OK);
    };
    const std::array<uint8_t, 64> seed{7};
    WalletStorageIdentity identity{WalletNetwork::Regtest,
                                   f.domain.genesis_wire, Hash{44}, 0};
    sql("PRAGMA synchronous=FULL;BEGIN IMMEDIATE;");
    WalletSnapshotStore::InitializeSchemaUnderTransaction(db);
    {
      WalletSnapshotStore store(db, identity, seed);
      Require(store.StageReplace(0, delivered.Encode()) == 1);
      sql("COMMIT;");
    }
    Require(sqlite3_close(db) == SQLITE_OK);
    Require(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    {
      WalletSnapshotStore store(db, identity, seed);
      auto saved = store.Read();
      Require(saved && saved->revision == 1);
      auto account = OrchardAccountState::Restore(
          saved->state, f.domain, fvk, 20001, funded.Scan().Checkpoint(),
          restoreLookups);
      Require(account.Scan().BalanceUna() == 5000 &&
              account.IssueReceiver(WalletScope::External).second == r2 &&
              account.Operations().Entries().at(id).transaction ==
                  f.Build().CanonicalBytes());
      Require(account.Observations() == funded.Observations());
      Require(account.Delivery() == delivered.Delivery());
      sql("BEGIN IMMEDIATE;");
      Require(store.StageReplace(1, rolled.Encode()) == 2);
      sql("ROLLBACK;");
      auto retained = store.Read();
      Require(retained && retained->revision == 1);
      Require(OrchardAccountState::Restore(retained->state, f.domain, fvk,
          20001, funded.Scan().Checkpoint(), restoreLookups).Delivery() == delivered.Delivery());
      Require(OrchardAccountState::Restore(retained->state, f.domain, fvk,
                                           20001, funded.Scan().Checkpoint(),
                                           restoreLookups)
                  .Observations() == funded.Observations());
      sql("BEGIN IMMEDIATE;");
      Require(store.StageReplace(1, rolled.Encode()) == 2);
      sql("COMMIT;");
    }
    Require(sqlite3_close(db) == SQLITE_OK);
    Require(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    {
      WalletSnapshotStore store(db, identity, seed);
      auto saved = store.Read();
      Require(saved && saved->revision == 2);
      auto after = OrchardAccountState::Restore(
          saved->state, f.domain, fvk, 20001, rewound.Scan().Checkpoint(),
          restoreLookups);
      Require(after.Observations().empty() &&
              after.Operations().Entries().at(id).transaction ==
                  f.Build().CanonicalBytes());
      Require(after.Delivery() == rolled.Delivery());
      Require(after.IssueReceiver(WalletScope::External).second == r2);
    }
    Require(sqlite3_close(db) == SQLITE_OK);
    // A real authenticated retained account supplies the immediate parent
    // after reopening. Current addresses and exact Ready bytes survive undo.
    auto parentPath=(cleanup.p/"parents.sqlite").string();
    Require(sqlite3_open(parentPath.c_str(),&db)==SQLITE_OK);
    sql("PRAGMA synchronous=FULL;BEGIN IMMEDIATE;");
    WalletSnapshotStore::InitializeSchemaUnderTransaction(db);
    { WalletSnapshotStore store(db,identity,seed);
      Require(store.StageReplaceRetaining(0,initial.Encode())==1);sql("COMMIT;BEGIN IMMEDIATE;");
      Require(store.StageReplaceRetaining(1,delivered.Encode())==2);sql("COMMIT;"); }
    Require(sqlite3_close(db)==SQLITE_OK);Require(sqlite3_open(parentPath.c_str(),&db)==SQLITE_OK);
    { WalletSnapshotStore store(db,identity,seed);
      auto parentBytes=store.ReadRetained(1);auto latest=store.Read();Require(latest&&latest->revision==2);
      auto parent=OrchardAccountState::Restore(parentBytes.state,f.domain,fvk,20001,initial.Scan().Checkpoint(),restoreLookups);
      auto current=OrchardAccountState::Restore(latest->state,f.domain,fvk,20001,delivered.Scan().Checkpoint(),restoreLookups);
      auto undone=current.RewindDelivery(removed,block,parent);
      Require(undone.Delivery()==rolled.Delivery()&&undone.Scan().BalanceUna()==0);
      Require(undone.Operations().Entries().at(id).transaction==delivered.Operations().Entries().at(id).transaction);
      Require(undone.IssueReceiver(WalletScope::External).second==r2);
      sql("BEGIN IMMEDIATE;");Require(store.StageReplaceRetaining(2,undone.Encode())==3);sql("COMMIT;");
      Require(store.ReadRetained(2).state.Bytes().size()==delivered.Encode().Bytes().size()); }
    Require(sqlite3_close(db)==SQLITE_OK);
    // A separately encrypted next snapshot is test-process input only.
    // The worker does the production StageReplace in a real SQLite transaction.
    auto nextPath = (cleanup.p / "next.sqlite").string();
    Require(sqlite3_open(nextPath.c_str(), &db) == SQLITE_OK);
    sql("PRAGMA synchronous=FULL;BEGIN IMMEDIATE;");
    WalletSnapshotStore::InitializeSchemaUnderTransaction(db);
    { WalletSnapshotStore store(db, identity, seed);
      Require(store.StageReplace(0, redelivered.Encode()) == 1); sql("COMMIT;"); }
    Require(sqlite3_close(db) == SQLITE_OK);
    for (const char *phase : {"before", "after"}) {
      std::vector<char *> args{argv[0], argv[1], nextPath.data(), path.data(),
                              const_cast<char *>(phase), nullptr};
      pid_t pid;
      Require(posix_spawn(&pid, argv[0], nullptr, nullptr, args.data(), environ) == 0);
      int status = 0;
      Require(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
      Require(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
      { WalletSnapshotStore store(db, identity, seed);
        const auto saved = store.Read();
        const bool committed = std::string(phase) == "after";
        const auto &expected = committed ? redelivered : rolled;
        Require(saved && saved->revision == (committed ? 3 : 2));
        auto actual = OrchardAccountState::Restore(saved->state, f.domain, fvk,
            20001, expected.Scan().Checkpoint(), restoreLookups);
        Require(actual.Delivery() == expected.Delivery() &&
                actual.Scan().BalanceUna() == (committed ? 5000 : 0) &&
                actual.Observations() == expected.Observations());
        Require(actual.IssueReceiver(WalletScope::External).second == r2 &&
                actual.Operations().Entries().at(id).transaction == f.Build().CanonicalBytes());
      }
      Require(sqlite3_close(db) == SQLITE_OK);
    }
    // Historical conflict/empty-checkpoint receipts also survive a fresh
    // process exiting immediately before or after the checked SQLite COMMIT.
    Require(sqlite3_open(path.c_str(), &db)==SQLITE_OK);
    sql("PRAGMA synchronous=FULL;BEGIN IMMEDIATE;");
    { WalletSnapshotStore store(db,identity,seed);Require(store.StageReplace(3,below.Encode())==4);sql("COMMIT;"); }
    Require(sqlite3_close(db)==SQLITE_OK);
    Require(sqlite3_open(nextPath.c_str(), &db)==SQLITE_OK);
    sql("PRAGMA synchronous=FULL;BEGIN IMMEDIATE;");
    { WalletSnapshotStore store(db,identity,seed);Require(store.StageReplace(1,historicalConflicted.Encode())==2);sql("COMMIT;"); }
    Require(sqlite3_close(db)==SQLITE_OK);
    for(const char* phase:{"before","after"}) {
      std::vector<char*> args{argv[0],argv[1],nextPath.data(),path.data(),const_cast<char*>(phase),nullptr};
      pid_t pid;Require(posix_spawn(&pid,argv[0],nullptr,nullptr,args.data(),environ)==0);
      int status=0;Require(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
      Require(sqlite3_open(path.c_str(), &db)==SQLITE_OK);
      { WalletSnapshotStore store(db,identity,seed);const auto saved=store.Read();
        const bool committed=std::string(phase)=="after";const auto& expected=committed?historicalConflicted:below;
        Require(saved && saved->revision==(committed?5:4));
        auto actual=OrchardAccountState::Restore(saved->state,f.domain,fvk,20001,expected.Scan().Checkpoint(),historicalLookups);
        Require(actual.Delivery()==expected.Delivery() && actual.Scan().Checkpoint()==expected.Scan().Checkpoint() && actual.Observations()==expected.Observations());
        Require(actual.Operations().Entries().at(id).transaction==ready.Operations().Entries().at(id).transaction);
        Require(actual.IssueReceiver(WalletScope::External).second==r2);
      }
      Require(sqlite3_close(db)==SQLITE_OK);
    }
#ifdef DINERO_TEST_BOUND_ACCOUNT
    // Exercise the bound consumer against real WalletManager identity/key
    // ownership. Explicit fixture enrollment is not production baseline proof.
    dinero::SelectParams(dinero::Chain::REGTEST);
    dinero::WalletManager manager(cleanup.p/"bound");manager.create("account");manager.open("account");
    const auto enroll=[&](const OrchardAccountState& baseline) {
      Require(manager.storeMasterSeed(std::vector<uint8_t>(seed.begin(),seed.end()),"",false));
      auto lease=manager.AcquireDatabaseLease();auto binding=lease->EnsureDeliveryIdentity();
      Hash walletId{};for(size_t i=0;i<32;++i)walletId[i]=static_cast<uint8_t>(std::stoul(binding.substr(7+2*i,2),nullptr,16));
      auto recoverySeed=lease->CopyRecoverySeed(lease->Session());
      Require(sqlite3_exec(lease->Database(),"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)==SQLITE_OK);
      WalletSnapshotStore::InitializeSchemaUnderTransaction(lease->Database());
      WalletSnapshotStore store(lease->Database(),{WalletNetwork::Regtest,f.domain.genesis_wire,walletId,0},recoverySeed->Bytes());
      Require(store.StageReplaceRetaining(0,baseline.Encode())==1);
      Require(sqlite3_exec(lease->Database(),"COMMIT",nullptr,nullptr,nullptr)==SQLITE_OK);
      return lease->Session();
    };
    using Owner=dinero::wallet::OrchardAccountDelivery;
    const Owner::Profile profile{f.domain,20001,0};
    Owner::RestorePoint before{ready.Scan().Checkpoint(),restoreLookups};
    Owner::RestorePoint after{delivered.Scan().Checkpoint(),restoreLookups};
    auto session=enroll(ready);
    auto connected=Owner::Connect(manager,session,profile,1,before,received,block,transition,auths);
    Require(connected.revision==2&&connected.account.Delivery()==delivered.Delivery()&&connected.account.Scan().BalanceUna()==5000);
    AccountReject([&]{(void)Owner::Disconnect(manager,session,profile,2,after,removed,block,2,before);});
    {auto lease=manager.AcquireDatabaseLease();Require(sqlite3_exec(lease->Database(),"CREATE TRIGGER reject_account BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'account failure');END",nullptr,nullptr,nullptr)==SQLITE_OK);}
    AccountReject([&]{(void)Owner::Disconnect(manager,session,profile,2,after,removed,block,1,before);});
    Require(Owner::Read(manager,session,profile,after).revision==2);
    {auto lease=manager.AcquireDatabaseLease();Require(sqlite3_exec(lease->Database(),"DROP TRIGGER reject_account",nullptr,nullptr,nullptr)==SQLITE_OK);}
    manager.open("account");AccountReject([&]{(void)Owner::Read(manager,session,profile,after);});
    session=manager.AcquireDatabaseLease()->Session();
    auto disconnected=Owner::Disconnect(manager,session,profile,2,after,removed,block,1,before);
    Require(disconnected.revision==3&&disconnected.account.Delivery()==rolled.Delivery()&&disconnected.account.Scan().BalanceUna()==0);
    Require(disconnected.account.Operations().Entries().at(id).transaction==ready.Operations().Entries().at(id).transaction);
    Require(disconnected.account.IssueReceiver(WalletScope::External).second==r2);
    auto connectedAgain=Owner::Connect(manager,session,profile,3,before,again,block,transition,auths);
    Require(connectedAgain.revision==4&&connectedAgain.account.Delivery()==redelivered.Delivery());
    manager.encryptWallet("bound-account-passphrase");
    AccountReject([&]{(void)Owner::Read(manager,session,profile,after);});
    manager.unlockWallet("bound-account-passphrase");
    Require(Owner::Read(manager,session,profile,after).revision==4);
    manager.create("history");manager.open("history");session=enroll(historyEmpty);
    Owner::RestorePoint historyPoint{historyEmpty.Scan().Checkpoint(),historicalLookups};
    const auto historical=Owner::Historical(manager,session,profile,1,historyPoint,historyDown);
    Require(historical.revision==2&&historical.account.Delivery()==below.Delivery());
    std::cout<<"Bound account owner: real keys/identity, note delivery, retained parent undo, SQL rollback, reopen, lock and historical effects passed\n";
#endif
    std::cout << "Orchard delivery: ordered receipt with note/rollback effects, legacy formats, "
                 "encrypted atomic rollback and fresh-process pre/post-commit recovery passed\n";
    std::cout << "Orchard account: atomic typed snapshot, address non-reuse "
                 "across restart/reorg/rescan, pending-byte preservation and "
                 "real received note passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
