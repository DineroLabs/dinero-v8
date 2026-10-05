#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
namespace parent_shielded = consensus::shielded;

struct FundedParentHistory {
    struct RestoreParams {
        ChainParams saved = Params();
        ~RestoreParams() { MutableParams() = saved; }
    } restore;
    static constexpr uint64_t deposit = 123456;
    static constexpr uint64_t fee = 1000;
    std::array<uint8_t, 32> signing_key{};
    std::array<uint8_t, 32> public_key{};
    std::vector<uint8_t> script;
    parent_shielded::Hash nullifier_key{}, randomness{}, diversifier{}, value{};
    parent_shielded::Hash note_commitment{};
    std::vector<Block> history;
    std::vector<arith_uint256> work;
    consensus::ConsensusUTXOSet coins;
    parent_shielded::CommitmentTree tree;
    parent_shielded::NullifierSet nullifiers;
    parent_shielded::AnchorHistory anchors;
    consensus::BlockValidator validator{&coins};

    static void Require(bool condition, const std::string& reason) {
        if (!condition) throw std::runtime_error("funded parent fixture: " + reason);
    }
    static parent_shielded::Hash Small(unsigned char n) {
        parent_shielded::Hash h{}; h.back() = n; return h;
    }
    explicit FundedParentHistory(bool same_block) {
        auto& p = MutableParams();
        Require(p.name == "regtest", "requires isolated regtest");
        p.orchard_activation_height = 103; p.orchard_branch_id = 1;
        p.shielded_activation_height = 1;
        p.shielded_input_binding_activation_height = 1;
        p.shielded_cv_binding_activation_height = 1;
        p.shielded_spend_auth_activation_height = 1;
        p.shielded_epoch_reset_height = UINT32_MAX;
        p.shielded_spend_auth_epoch_reset_height = UINT32_MAX;
        p.state_commitment_activation_height = 1;
        // Original consensus script, maturity, subsidy, proof and header checks
        // remain enabled. The fixture selects an independent regtest boundary.
        Require(nullifiers.Open(":memory:") == parent_shielded::NullifierSet::OpenResult::Ok,
            "nullifier store");
        validator.setValidationMode(consensus::ValidationMode::STATEFUL);
        validator.setShieldedState(&tree, &nullifiers, &anchors);
        signing_key.back() = 1; int parity = -1;
        Require(TaprootKeys::DeriveXOnlyPubkey(signing_key, public_key, parity) && parity == 0,
            "even-y test signing scalar");
        script = {0x51, 0x20}; script.insert(script.end(), public_key.begin(), public_key.end());
        nullifier_key = Small(2); randomness = Small(3); diversifier = Small(4);
        for (unsigned i = 0; i < 8; ++i) value[31-i] = uint8_t(deposit >> (8*i));
        const auto ownership = parent_shielded::AuthRecipientCommitmentKey(public_key,
            parent_shielded::PoseidonHash2(nullifier_key, parent_shielded::NullifierKeyTag()));
        note_commitment = parent_shielded::NoteCommitment(diversifier, ownership, value, randomness);
        parent_shielded::OutputWitness output_witness{};
        output_witness.value = value; output_witness.public_key = ownership;
        output_witness.randomness = randomness; output_witness.d = diversifier;
        output_witness.rcv = Small(5);
        parent_shielded::OutputPublicInputs output_public{};
        output_public.commitment = note_commitment;
        Require(parent_shielded::PedersenCommit(output_witness.rcv, deposit, output_public.cv) ==
            parent_shielded::PedersenResult::Ok, "deposit commitment");
        parent_shielded::PlannedOutput planned_output{};
        planned_output.commitment = note_commitment; planned_output.value_una = deposit;
        planned_output.rcv = output_witness.rcv; planned_output.nonce = Small(6);
        // Opaque recipient ciphertext is outside retirement accounting. Proofs,
        // public amounts and signatures below are genuine, not stubbed.
        planned_output.encrypted_note = std::vector<uint8_t>(32, 0x42);
        planned_output.output_proof = parent_shielded::ProveOutput(output_witness, output_public,
            nullptr, true, true);
        Require(!planned_output.output_proof.empty(), "genuine output proof");

        history.push_back(SelectedGenesis());
        SeedDirect(coins);
        work.push_back(GetBlockProof(history.front().header.difficulty));
        for (uint32_t height = 1; height <= 102; ++height) {
            Block body;
            body.header.version = 1;
            body.header.prev_block_hash = history.back().GetHash();
            body.header.timestamp = history.front().header.timestamp + height * 120;
            body.header.difficulty = 0x207fffff;
            body.header.ZeroReserved();
            body.vtx.push_back(MakeCoinbase(height));
            if (height == 1) body.vtx.front().vout.front().scriptPubKey = script;
            std::vector<parent_shielded::ShieldedBundle> bundles;
            if (height == 101) {
                OutPoint point(history[1].vtx.front().GetTxid(), 0);
                const auto* found = coins.GetCoin(point);
                Require(found && found->isCoinbase && found->height == 1, "real mature funding coin");
                auto funding = *found;
                if (same_block) {
                    auto intermediate = TransparentSpend(point, funding, funding.value.GetUna());
                    Sign(intermediate, point, funding);
                    body.vtx.push_back(intermediate);
                    point = OutPoint(intermediate.GetTxid(), 0);
                    funding = consensus::UTXOEntry(intermediate.vout.front().value, script, height, false);
                }
                Require(funding.value.GetUna() > deposit + fee, "funded deposit amount");
                auto tx = TransparentSpend(point, funding, funding.value.GetUna() - deposit - fee);
                tx.version = Transaction::TX_VERSION_SHIELDED_V2; tx.SetExplicitFee(fee);
                parent_shielded::ShieldedBundle bundle;
                Require(parent_shielded::BuildShieldedBundle({}, {planned_output},
                    parent_shielded::ComputeShieldedTxSighash(tx), bundle) ==
                    parent_shielded::BundleBuildResult::Ok, "deposit bundle");
                tx.shielded_bundle_bytes = parent_shielded::SerializeShieldedBundle(bundle);
                Sign(tx, point, funding);
                body.vtx.push_back(std::move(tx)); bundles.push_back(std::move(bundle));
            } else if (height == 102) {
                const auto path = tree.GetAuthPath(0);
                Require(path.has_value() && tree.Size() == 1, "deposited note exists");
                parent_shielded::SpendWitness witness{};
                witness.secret_key = signing_key; witness.nullifier_key = nullifier_key;
                witness.leaf_index = 0; witness.value = value; witness.randomness = randomness;
                witness.d = diversifier; witness.rcv = Small(7); witness.merkle_path = path->siblings;
                parent_shielded::SpendPublicInputs inputs{};
                inputs.nullifier = parent_shielded::ComputeNullifier(nullifier_key, 0);
                inputs.anchor = tree.Root();
                Require(parent_shielded::PedersenCommit(witness.rcv, deposit, inputs.cv) ==
                    parent_shielded::PedersenResult::Ok, "withdrawal commitment");
                parent_shielded::PlannedSpend spend{};
                spend.nullifier = inputs.nullifier; spend.anchor = inputs.anchor;
                spend.value_una = deposit; spend.rcv = witness.rcv; spend.nonce = Small(8);
                spend.spend_proof = parent_shielded::ProveSpend(witness, inputs, nullptr, true, true, true);
                Require(!spend.spend_proof.empty(), "genuine spend-authority proof");
                Transaction tx; tx.version = Transaction::TX_VERSION_SHIELDED_V2;
                tx.SetExplicitFee(fee); tx.vout.emplace_back(AmountUna::Una(deposit-fee), script);
                parent_shielded::ShieldedBundle bundle;
                Require(parent_shielded::BuildShieldedBundle({spend}, {},
                    parent_shielded::ComputeShieldedTxSighash(tx), bundle) ==
                    parent_shielded::BundleBuildResult::Ok, "withdrawal bundle");
                tx.shielded_bundle_bytes = parent_shielded::SerializeShieldedBundle(bundle);
                body.vtx.push_back(std::move(tx)); bundles.push_back(std::move(bundle));
            }
            std::vector<parent_shielded::NullifierEntry> entries;
            Require(nullifiers.ForEach([&](uint32_t h, const uint8_t* bytes) {
                parent_shielded::NullifierEntry e; e.height = h;
                std::copy(bytes, bytes+32, e.nullifier.begin()); entries.push_back(e); return true;
            }), "checked prior nullifier inventory");
            const auto predicted = parent_shielded::PredictPostBlockShieldedRoot(bundles, height,
                p.shielded_epoch_reset_height, p.shielded_spend_auth_epoch_reset_height,
                p.shielded_activation_height, tree, std::move(entries), anchors);
            Require(bool(predicted), "post-block commitment prediction");
            body.vtx.front().vout.emplace_back(AmountUna::Zero(), consensus::BuildStateCommitmentScript(*predicted));
            body.header.merkle_root = consensus::ComputeMerkleRoot(body.vtx);
            std::string error;
            Require(validator.ComputeUtreexoRootPure(body, height, body.header.utreexo_root, error),
                "forest construction: " + error);
            consensus::BlockUndo undo;
            Require(validator.ConnectBlock(body, height, body.GetHash(), undo, error),
                "full forward connection: " + error);
            work.push_back(work.back() + GetBlockProof(body.header.difficulty));
            history.push_back(std::move(body));
        }
    }
    Transaction TransparentSpend(const OutPoint& point, const consensus::UTXOEntry&, uint64_t amount) {
        Transaction tx; tx.version = 2; tx.witness_version = 1;
        TxInput input; input.prevout = {point.txid, point.vout}; input.sequence = 0xfffffffe;
        tx.vin.push_back(input); tx.vout.emplace_back(AmountUna::Una(amount), script); return tx;
    }
    void Sign(Transaction& tx, const OutPoint& point, const consensus::UTXOEntry& coin) {
        CanonicalWalletUTXO input;
        input.txid = point.txid.AsUint256(); input.vout = point.vout; input.value = coin.value;
        input.spk = coin.scriptPubKey; input.height = coin.height; input.is_coinbase = coin.isCoinbase;
        const auto bytes = TaprootTxSigner::ComputeTaprootSighash(tx, 0, {input}, TaprootTxSigner::SIGHASH_DEFAULT);
        Require(bytes.size() == 32, "transparent signature message");
        std::array<uint8_t, 32> message{}; std::copy(bytes.begin(), bytes.end(), message.begin());
        std::array<uint8_t, 64> signature{};
        Require(TaprootKeys::SignSchnorr(signature, message, signing_key), "transparent signature");
        tx.vin.front().witness = {std::vector<uint8_t>(signature.begin(), signature.end())};
    }
    void CheckRetirement() {
        for (uint32_t height : {101u, 102u}) {
            MutableParams().orchard_activation_height = height + 1;
            OrchardParentReplay replay({height, history[height].GetHash(), work[height]}, parent_replay_limits);
            for (uint32_t h = 0; h <= height; ++h) replay.Append(history[h], h, work[h]);
            replay.Finish();
            EXPECT_EQ(replay.Record().retired_value, height == 101 ? deposit : 0u);
            EXPECT_EQ(replay.Record().tree_size, 1u);
            EXPECT_EQ(replay.Record().nullifier_count, height == 101 ? 0u : 1u);
            EXPECT_EQ(replay.Accounting().shielded_transactions, height == 101 ? 1u : 2u);
            EXPECT_EQ(replay.Record().boundary_parent, history[height].GetHash());
            const auto commitment = consensus::FindStateCommitment(history[height].vtx.front());
            ASSERT_EQ(commitment.status, consensus::StateCommitmentStatus::Ok);
            EXPECT_EQ(replay.Record().legacy_state_root, commitment.root);
        }
    }
};
}
TEST(OrchardParentReplay, RealMatureFundingShieldAndUnshieldAccounting) {
    FundedParentHistory history(false); history.CheckRetirement();
}
TEST(OrchardParentReplay, RealSameBlockFundingShieldAndUnshieldAccounting) {
    FundedParentHistory history(true); history.CheckRetirement();
}
#endif
