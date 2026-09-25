#include "orchard_block_test_fixture.h"
#include "wallet/orchard_account_state.h"
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
    Require(argc == 2);
    Fixture f(argv[1]);
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
    auto bytes = funded.Encode();
    auto restored = OrchardAccountState::Restore(bytes, f.domain, fvk, 20001,
                                                 funded.Scan().Checkpoint(),
                                                 restoreLookups);
    Require(restored.Scan().BalanceUna() == 5000 &&
            restored.Operations().Entries().at(id).transaction ==
                f.Build().CanonicalBytes());
    Require(restored.Observations() == funded.Observations());
    Require(restored.IssueReceiver(WalletScope::External).second == r2);
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
    old.resize(old.size() - 4);
    Require(OrchardAccountState::RestoreForRescan(WalletStateBytes(old),
                                                  f.domain, fvk, 20001, H(1))
                .Observations()
                .empty());
    auto badReceipt =
        std::vector<uint8_t>(bytes.Bytes().begin(), bytes.Bytes().end());
    const auto receiptStart = badReceipt.size() - 101;
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
      Require(store.StageReplace(0, bytes) == 1);
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
      sql("BEGIN IMMEDIATE;");
      Require(store.StageReplace(1, rewound.Encode()) == 2);
      sql("ROLLBACK;");
      auto retained = store.Read();
      Require(retained && retained->revision == 1);
      Require(OrchardAccountState::Restore(retained->state, f.domain, fvk,
                                           20001, funded.Scan().Checkpoint(),
                                           restoreLookups)
                  .Observations() == funded.Observations());
      sql("BEGIN IMMEDIATE;");
      Require(store.StageReplace(1, rewound.Encode()) == 2);
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
      Require(after.IssueReceiver(WalletScope::External).second == r2);
    }
    Require(sqlite3_close(db) == SQLITE_OK);
    std::cout << "Orchard account: atomic typed snapshot, address non-reuse "
                 "across restart/reorg/rescan, pending-byte preservation and "
                 "real received note passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
