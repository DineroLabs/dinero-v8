#pragma once
// v8.2.4 (#939 follow-up): Orchard spends for native wallets on a full-archive
// v3 node. Included after orchard_service_proofs_checks.h (ServiceProofTerminal)
// and wallet_v3_recovery_route_checks.h (NativeV3IndexScope). Orchard-origin only.

namespace dinero {

// A native-complete wallet on v3 spends a delivered Orchard note through the
// real RPC route: queue -> proof -> finish (authorize at the v3 source, commit
// Ready, admit to the mempool) -> mine -> v3 delivery. The account's balance
// drops by exactly the unshielded amount plus fee, the transparent receipts
// stay certified, and the native class is untouched.
TEST(WalletV3NativeOrchardSpend, NativeWalletSpendsDeliveredNoteOnV3) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-orchard-spend-index");
    VerifyCatalogReuse verify_reuse;
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    f.context.tx_ingress=f.f.ingress.get();
    auto ctx=f.RequestContext();
    const auto created=rpc_context_wallet_orchard_createaccount(ctx,OrchardCreationRequest());
    ASSERT_FALSE(created.isMember("error")) << created.toStyledString();
    const auto [body,bundle]=f.Shield(f.AccountKeys());(void)bundle;
    (void)f.Mine(body);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    ASSERT_EQ(f.Account().account.Scan().BalanceUna(),500000u);

    // Unshield 100000 to an external transparent script, fee 10000.
    const std::vector<uint8_t> program(32,0x5a);
    din::Json request,output;
    request["account"]=Json::UInt64(3);
    request["expected_revision"]=Json::UInt64(f.Account().revision);
    std::vector<uint8_t> id(32);id[0]=0x91;
    request["request_id"]=util::hex(id);
    request["fee_una"]=Json::UInt64(10000);
    request["payments"]=din::arr();request["outputs"]=din::arr();
    output["address"]=bech32::Encode("rdin",1,program,bech32::Encoding::BECH32M);
    output["amount_una"]=Json::UInt64(100000);
    request["outputs"].append(output);
    const auto queued=rpc_context_wallet_orchard_queuespend(ctx,request);
    ASSERT_FALSE(queued.isMember("error")) << queued.toStyledString();
    EXPECT_EQ(queued["durable_state"].asString(),"reserved");
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        orchard::Hash op{};std::copy(id.begin(),id.end(),op.begin());
        ASSERT_EQ(ServiceProofTerminal(use->OrchardProofs(),op),
            wallet::OrchardProofJobs::State::Succeeded);
    }
    din::Json finish;finish["account"]=Json::UInt64(3);finish["request_id"]=request["request_id"];
    const auto finished=rpc_context_wallet_orchard_finishspend(ctx,finish);
    ASSERT_FALSE(finished.isMember("error")) << finished.toStyledString();
    EXPECT_TRUE(finished["admitted"].asBool()) << finished.toStyledString();
    const auto txid=uint256::FromHexUnsafe(finished["txid"].asString());
    const auto pooled=f.f.ingress->mempool().getMempoolEntry(txid);
    ASSERT_TRUE(bool(pooled));
    // v8.2.7: the queue-time price of a 2-action unshield with one transparent
    // output is the admitted transaction's exact size.
    std::vector<uint8_t> script{0x51,0x20};script.insert(script.end(),program.begin(),program.end());
    EXPECT_EQ(pooled->tx.GetVirtualSize(),
        orchard::InputlessEnvelopeBytes({{100000,script}},10000,2));
    ASSERT_TRUE(f.Mine(pooled->tx));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    EXPECT_EQ(f.Account().account.Scan().BalanceUna(),390000u);
    const auto transparent=rpc_context_wallet_getbalance(ctx,din::obj());
    EXPECT_FALSE(transparent.isMember("error")) << transparent.toStyledString();
    auto use=WalletService::AcquireWalletUse(f.wallet);
    EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->NativeV3TransparentState(),
        WalletManager::NativeV3State::Complete);
    EXPECT_FALSE(HasLegacyStateColumn(use->Wallet()));
    f.context.tx_ingress=nullptr;
}

// Round trip on v3: unshield to the wallet's own pooled Taproot address, see
// the coin through the transparent v3 receipt, then shield it back into the
// Orchard account. Selection reads only receipt-certified coins; the shield's
// transparent change must not invalidate or escape the sealed receipt domain.
TEST(WalletV3NativeOrchardSpend, NativeWalletRoundTripsUnshieldAndShieldOnV3) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-shield-index");
    VerifyCatalogReuse verify_reuse;
    std::string error;
    const auto deliver=[&]{
        ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
            WalletService::CanonicalRecovery::AppliedPrefix) << error;
    };
    deliver();
    f.context.tx_ingress=f.f.ingress.get();
    auto ctx=f.RequestContext();
    const auto created=rpc_context_wallet_orchard_createaccount(ctx,OrchardCreationRequest());
    ASSERT_FALSE(created.isMember("error")) << created.toStyledString();
    const auto [body,bundle]=f.Shield(f.AccountKeys());(void)bundle;
    (void)f.Mine(body);
    deliver();
    const auto own=rpc_context_wallet_getnewaddress(ctx,din::obj());
    ASSERT_FALSE(own.isMember("error")) << own.toStyledString();
    const auto own_address=own.isMember("address")?own["address"].asString():own.asString();
    const auto run=[&](bool shield,uint8_t tag,const din::Json& recipient){
        din::Json request;
        request["account"]=Json::UInt64(3);
        request["expected_revision"]=Json::UInt64(f.Account().revision);
        std::vector<uint8_t> id(32);id[0]=tag;request["request_id"]=util::hex(id);
        request["fee_una"]=Json::UInt64(10000);request["payments"]=din::arr();
        if(shield)request["payments"].append(recipient);
        else{request["outputs"]=din::arr();request["outputs"].append(recipient);}
        const auto queued=shield?rpc_context_wallet_orchard_queueshield(ctx,request)
                                :rpc_context_wallet_orchard_queuespend(ctx,request);
        EXPECT_FALSE(queued.isMember("error")) << queued.toStyledString();
        if(queued.isMember("error"))return false;
        {
            auto use=WalletService::AcquireWalletUse(f.wallet);
            orchard::Hash op{};std::copy(id.begin(),id.end(),op.begin());
            EXPECT_EQ(ServiceProofTerminal(use->OrchardProofs(),op),wallet::OrchardProofJobs::State::Succeeded);
        }
        din::Json finish;finish["account"]=Json::UInt64(3);finish["request_id"]=request["request_id"];
        const auto finished=shield?rpc_context_wallet_orchard_finishshield(ctx,shield?request:finish)
                                  :rpc_context_wallet_orchard_finishspend(ctx,finish);
        EXPECT_FALSE(finished.isMember("error")) << finished.toStyledString();
        EXPECT_TRUE(finished["admitted"].asBool()) << finished.toStyledString();
        if(!finished["admitted"].asBool())return false;
        const auto pooled=f.f.ingress->mempool().getMempoolEntry(uint256::FromHexUnsafe(finished["txid"].asString()));
        EXPECT_TRUE(bool(pooled));if(!pooled)return false;
        EXPECT_TRUE(f.Mine(pooled->tx));
        return true;
    };
    din::Json out;out["address"]=own_address;out["amount_una"]=Json::UInt64(100000);
    ASSERT_TRUE(run(false,0x93,out));
    deliver();
    EXPECT_EQ(f.Account().account.Scan().BalanceUna(),390000u);
    const auto funded=rpc_context_wallet_getbalance(ctx,din::obj());
    ASSERT_FALSE(funded.isMember("error")) << funded.toStyledString();
    EXPECT_NEAR(funded["spendable"].asDouble()*1e8,100000.0,0.5) << funded.toStyledString();
    const auto address_rows=[&]{auto use=WalletService::AcquireWalletUse(f.wallet);
        return NativeTestCount(use->Wallet(),"SELECT count(*) FROM addresses");};
    const auto rows_before=address_rows();
    din::Json pay;pay["address"]=created["address"];pay["amount_una"]=Json::UInt64(20000);
    ASSERT_TRUE(run(true,0x94,pay));
    // The shield's 70000 change went to a pooled change row: nothing derived.
    EXPECT_EQ(address_rows(),rows_before);
    deliver();
    EXPECT_EQ(f.Account().account.Scan().BalanceUna(),410000u);
    const auto after=rpc_context_wallet_getbalance(ctx,din::obj());
    ASSERT_FALSE(after.isMember("error")) << after.toStyledString();
    EXPECT_NEAR(after["spendable"].asDouble()*1e8,70000.0,0.5) << after.toStyledString();
    // A reorg un-confirms the wallet's own confirmed shield (the owner's Hotel
    // wallet holds exactly such a shield): delivery rewinds through it, and
    // every reuse of a remembered catalog is also restored and compared
    // (VerifyCatalogReuse above).
    // An idle pass first, so the reorg meets an armed idle skip: the moved
    // head must bring the complete pass (and its reconcile) back.
    const auto skipped=RuntimeWalletRecovery::IdleCatalogPassesSkipped();
    deliver();
    EXPECT_EQ(RuntimeWalletRecovery::IdleCatalogPassesSkipped(),skipped+1) << "idle pass was not skipped";
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    deliver();
    EXPECT_EQ(RuntimeWalletRecovery::IdleCatalogPassesSkipped(),skipped+1) << "reorg pass was skipped";
    EXPECT_EQ(f.Account().account.Scan().BalanceUna(),390000u) << "un-confirmed shield still counted";
    EXPECT_EQ(f.Account().account.Delivery().sequence,NativeOrchardHead(f));
    auto use=WalletService::AcquireWalletUse(f.wallet);
    EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->NativeV3TransparentState(),
        WalletManager::NativeV3State::Complete);
    f.context.tx_ingress=nullptr;
}

// A confirmed shield stays in every later revision's operation queue, so one
// catalog capture meets it once per retained parent. Its history row is the
// same row in the same transaction each time: one capture checks it once.
TEST(WalletV3NativeOrchardSpend, CaptureChecksAShieldHistoryOncePerCapture) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-shield-history-once-index");
    std::string error;
    const auto deliver=[&]{
        ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
            WalletService::CanonicalRecovery::AppliedPrefix) << error;
    };
    deliver();
    f.context.tx_ingress=f.f.ingress.get();
    auto ctx=f.RequestContext();
    const auto created=rpc_context_wallet_orchard_createaccount(ctx,OrchardCreationRequest());
    ASSERT_FALSE(created.isMember("error")) << created.toStyledString();
    const auto [body,bundle]=f.Shield(f.AccountKeys());(void)bundle;
    (void)f.Mine(body);
    deliver();
    const auto own=rpc_context_wallet_getnewaddress(ctx,din::obj());
    ASSERT_FALSE(own.isMember("error")) << own.toStyledString();
    const auto own_address=own.isMember("address")?own["address"].asString():own.asString();
    const auto run=[&](bool shield,uint8_t tag,const din::Json& recipient){
        din::Json request;
        request["account"]=Json::UInt64(3);
        request["expected_revision"]=Json::UInt64(f.Account().revision);
        std::vector<uint8_t> id(32);id[0]=tag;request["request_id"]=util::hex(id);
        request["fee_una"]=Json::UInt64(10000);request["payments"]=din::arr();
        if(shield)request["payments"].append(recipient);
        else{request["outputs"]=din::arr();request["outputs"].append(recipient);}
        const auto queued=shield?rpc_context_wallet_orchard_queueshield(ctx,request)
                                :rpc_context_wallet_orchard_queuespend(ctx,request);
        EXPECT_FALSE(queued.isMember("error")) << queued.toStyledString();
        if(queued.isMember("error"))return false;
        {
            auto use=WalletService::AcquireWalletUse(f.wallet);
            orchard::Hash op{};std::copy(id.begin(),id.end(),op.begin());
            EXPECT_EQ(ServiceProofTerminal(use->OrchardProofs(),op),wallet::OrchardProofJobs::State::Succeeded);
        }
        din::Json finish;finish["account"]=Json::UInt64(3);finish["request_id"]=request["request_id"];
        const auto finished=shield?rpc_context_wallet_orchard_finishshield(ctx,request)
                                  :rpc_context_wallet_orchard_finishspend(ctx,finish);
        EXPECT_TRUE(finished["admitted"].asBool()) << finished.toStyledString();
        if(!finished["admitted"].asBool())return false;
        const auto pooled=f.f.ingress->mempool().getMempoolEntry(uint256::FromHexUnsafe(finished["txid"].asString()));
        EXPECT_TRUE(bool(pooled));if(!pooled)return false;
        EXPECT_TRUE(f.Mine(pooled->tx));
        return true;
    };
    din::Json out;out["address"]=own_address;out["amount_una"]=Json::UInt64(100000);
    ASSERT_TRUE(run(false,0x95,out));
    deliver();
    din::Json pay;pay["address"]=created["address"];pay["amount_una"]=Json::UInt64(20000);
    ASSERT_TRUE(run(true,0x96,pay));
    deliver();
    ASSERT_EQ(f.Account().account.Scan().BalanceUna(),410000u);
    // Each delivered block leaves one more retained parent carrying the shield.
    constexpr int kBlocks=8;
    for(int i=0;i<kBlocks;++i){(void)f.MineEmpty();deliver();}
    // Force the complete pass at the head (no idle skip) and count.
    RuntimeWalletRecovery::SetIdleCatalogFullPassIntervalForTest(std::chrono::milliseconds(0));
    const auto captures=wallet::OrchardAccountDelivery::CatalogCaptures();
    const auto checks=WalletManager::DatabaseLease::ShieldHistoryValidations();
    deliver();
    RuntimeWalletRecovery::SetIdleCatalogFullPassIntervalForTest(std::chrono::milliseconds(60000));
    const auto captured=wallet::OrchardAccountDelivery::CatalogCaptures()-captures;
    const auto checked=WalletManager::DatabaseLease::ShieldHistoryValidations()-checks;
    ASSERT_GT(captured,0u) << "the forced pass captured nothing";
    EXPECT_EQ(checked,captured) << "one shield history row checked " << checked << " times in "
        << captured << " captures";
    EXPECT_EQ(f.Account().account.Delivery().sequence,NativeOrchardHead(f));
    f.context.tx_ingress=nullptr;
}

TEST(WalletV3NativeOrchardSpend, ColdRestoreReadsEachParentLinkOnce) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-cold-restore-links-index");
    std::string error;
    const auto deliver=[&]{
        ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
            WalletService::CanonicalRecovery::AppliedPrefix) << error;
    };
    deliver();
    f.context.tx_ingress=f.f.ingress.get();
    auto ctx=f.RequestContext();
    const auto created=rpc_context_wallet_orchard_createaccount(ctx,OrchardCreationRequest());
    ASSERT_FALSE(created.isMember("error")) << created.toStyledString();
    const auto [body,bundle]=f.Shield(f.AccountKeys());(void)bundle;
    (void)f.Mine(body);
    deliver();
    const auto own=rpc_context_wallet_getnewaddress(ctx,din::obj());
    ASSERT_FALSE(own.isMember("error")) << own.toStyledString();
    const auto own_address=own.isMember("address")?own["address"].asString():own.asString();
    const auto run=[&](bool shield,uint8_t tag,const din::Json& recipient){
        din::Json request;
        request["account"]=Json::UInt64(3);
        request["expected_revision"]=Json::UInt64(f.Account().revision);
        std::vector<uint8_t> id(32);id[0]=tag;request["request_id"]=util::hex(id);
        request["fee_una"]=Json::UInt64(10000);request["payments"]=din::arr();
        if(shield)request["payments"].append(recipient);
        else{request["outputs"]=din::arr();request["outputs"].append(recipient);}
        const auto queued=shield?rpc_context_wallet_orchard_queueshield(ctx,request)
                                :rpc_context_wallet_orchard_queuespend(ctx,request);
        EXPECT_FALSE(queued.isMember("error")) << queued.toStyledString();
        if(queued.isMember("error"))return false;
        {
            auto use=WalletService::AcquireWalletUse(f.wallet);
            orchard::Hash op{};std::copy(id.begin(),id.end(),op.begin());
            EXPECT_EQ(ServiceProofTerminal(use->OrchardProofs(),op),wallet::OrchardProofJobs::State::Succeeded);
        }
        din::Json finish;finish["account"]=Json::UInt64(3);finish["request_id"]=request["request_id"];
        const auto finished=shield?rpc_context_wallet_orchard_finishshield(ctx,request)
                                  :rpc_context_wallet_orchard_finishspend(ctx,finish);
        EXPECT_TRUE(finished["admitted"].asBool()) << finished.toStyledString();
        if(!finished["admitted"].asBool())return false;
        const auto pooled=f.f.ingress->mempool().getMempoolEntry(uint256::FromHexUnsafe(finished["txid"].asString()));
        EXPECT_TRUE(bool(pooled));if(!pooled)return false;
        EXPECT_TRUE(f.Mine(pooled->tx));
        return true;
    };
    din::Json out;out["address"]=own_address;out["amount_una"]=Json::UInt64(100000);
    ASSERT_TRUE(run(false,0x95,out));
    deliver();
    din::Json pay;pay["address"]=created["address"];pay["amount_una"]=Json::UInt64(20000);
    ASSERT_TRUE(run(true,0x96,pay));
    deliver();
    ASSERT_EQ(f.Account().account.Scan().BalanceUna(),410000u);
    // Many retained revisions, each restored at its own tip: every lookup walks
    // back from that tip. Force one restore (no remembered catalog) and count
    // block nodes read from the replay store. The view's links may already be
    // warm here; the check is that reads no longer grow as lookups x depth.
    constexpr int kBlocks=40;
    for(int i=0;i<kBlocks;++i){(void)f.MineEmpty();deliver();}
    wallet::OrchardAccountDelivery::ForgetCatalogRecovery();
    RuntimeWalletRecovery::SetIdleCatalogFullPassIntervalForTest(std::chrono::milliseconds(0));
    const auto restores=wallet::OrchardAccountDelivery::CatalogRecoveryRestores();
    const auto reads=RuntimeAccountReplay::NodeReads();
    deliver();
    RuntimeWalletRecovery::SetIdleCatalogFullPassIntervalForTest(std::chrono::milliseconds(60000));
    const auto restored=wallet::OrchardAccountDelivery::CatalogRecoveryRestores()-restores;
    const auto read=RuntimeAccountReplay::NodeReads()-reads;
    const auto height=f.Account().account.Scan().Checkpoint().height;
    ASSERT_GT(restored,0u) << "nothing was restored";
    // Each block's parent link is read from the store at most once per view;
    // the rest is about one node per lookup. Walking every lookup down from
    // its tip read ~6,400 nodes here (44 per block), growing with depth.
    EXPECT_EQ(restored,1u);
    EXPECT_LE(read,4*uint64_t(height)) << read << " node reads for a restore over a chain of height " << height;
    EXPECT_EQ(f.Account().account.Delivery().sequence,NativeOrchardHead(f));
    f.context.tx_ingress=nullptr;
}

// One shield that pays the account three separate notes, from the fixture's
// funded coin (the same coin and signing as OrchardCycleFixture::Shield).
MempoolTransaction ShieldThreeNotes(OrchardCreationFixture& f,const orchard::WalletKeys& receiver,uint64_t each) {
    const OutPoint point(f.f.blocks[1].vtx.front().GetTxid(),0);
    const auto& coin=f.f.replay->ProvenUtxos().at(point);
    orchard::EnvelopeInput input{};
    std::copy(point.txid.AsUint256().begin(),point.txid.AsUint256().end(),input.txid_wire.begin());
    input.output_index=0;input.sequence=UINT32_MAX;
    constexpr uint64_t fee=100000;
    const std::vector<orchard::TransparentOutput> outputs{{coin.value.GetUna()-fee-3*each,f.f.script}};
    std::vector<orchard::WalletPayment> payments;
    for(int i=0;i<3;++i)payments.push_back({each,receiver.Receiver(orchard::WalletScope::External,{}),{}});
    const std::vector<orchard::ResolvedInput> inputs{{input.txid_wire,0,input.sequence,coin.value.GetUna(),f.f.script}};
    const auto bundle=orchard::WalletBundlePlan::PrepareShield(receiver,payments)
        .Prove(orchard::SigningContext::Create(f.Domain(),0,inputs,outputs,fee));
    const auto draft=orchard::TransactionEnvelope::Create(0,{input},outputs,fee,bundle.Bytes());
    FirstBoundaryView view(*f.f.replay);
    const auto snapshot=consensus::OrchardCoinSnapshot::ResolveUnderChainstateLock(draft,view);
    const auto digest=consensus::OrchardTransparentSigningDigest(snapshot,f.Domain(),0);
    std::unique_ptr<secp256k1_context,decltype(&secp256k1_context_destroy)> signing(
        secp256k1_context_create(SECP256K1_CONTEXT_NONE),secp256k1_context_destroy);
    secp256k1_keypair pair;OrchardAdmissionFixture::Require(secp256k1_keypair_create(signing.get(),&pair,f.f.secret.data()));
    std::vector<uint8_t> signature(64);orchard::Hash aux{};
    OrchardAdmissionFixture::Require(secp256k1_schnorrsig_sign32(signing.get(),signature.data(),digest.data(),&pair,aux.data()));
    input.witness={signature};
    return MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::Create(0,{input},outputs,fee,bundle.Bytes()));
}

// v8.2.7 fee floor: a private send that needs three notes has three actions
// (about 12.4 kvB), so the 10,000 una fee every client sends today is below
// the mempool's 1 una/vB floor for it. Before this fix the operation was
// signed, admission refused it ("Orchard fee rate below minimum"), every
// retry resubmitted the same bytes, and all 450,000 una stayed reserved with
// no way to release them. Now the queue refuses it first, with the exact fee
// it needs, and leaves nothing behind; that fee is then admitted.
TEST(WalletV3NativeOrchardSpend, ThreeNoteSendBelowFeeFloorRefusedBeforeReserving) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-orchard-fee-floor-index");
    std::string error;
    const auto deliver=[&]{
        ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
            WalletService::CanonicalRecovery::AppliedPrefix) << error;
    };
    deliver();
    f.context.tx_ingress=f.f.ingress.get();
    auto ctx=f.RequestContext();
    const auto created=rpc_context_wallet_orchard_createaccount(ctx,OrchardCreationRequest());
    ASSERT_FALSE(created.isMember("error")) << created.toStyledString();
    (void)f.Mine(ShieldThreeNotes(f,f.AccountKeys(),150000));
    deliver();
    ASSERT_EQ(f.Account().account.Scan().BalanceUna(),450000u);
    ASSERT_EQ(f.f.ingress->mempool().getMinFeeRate(),1.0);

    const auto spend=[&](uint8_t tag,uint64_t fee){
        din::Json request,pay;
        request["account"]=Json::UInt64(3);
        request["expected_revision"]=Json::UInt64(f.Account().revision);
        std::vector<uint8_t> id(32);id[0]=tag;request["request_id"]=util::hex(id);
        request["fee_una"]=Json::UInt64(fee);
        request["payments"]=din::arr();request["outputs"]=din::arr();
        pay["address"]=created["address"];pay["amount_una"]=Json::UInt64(400000);
        request["payments"].append(pay);
        return std::pair{request,rpc_context_wallet_orchard_queuespend(ctx,request)};
    };
    const auto nothing_left=[&](const char* when){
        const auto balance=rpc_context_wallet_orchard_getbalance(ctx,OrchardOperationListRequest());
        EXPECT_EQ(balance["reserved_confirmed_una"].asUInt64(),0u) << when << balance.toStyledString();
        EXPECT_EQ(balance["unreserved_confirmed_una"].asUInt64(),450000u) << when;
        const auto listed=rpc_context_wallet_orchard_listoperations(ctx,OrchardOperationListRequest());
        ASSERT_TRUE(listed["operations"].isArray()) << when << listed.toStyledString();
        EXPECT_TRUE(listed["operations"].empty()) << when << listed.toStyledString();
    };
    const auto [low_request,low]=spend(0xa7,10000);(void)low_request;
    ASSERT_TRUE(low.isMember("error")) << low.toStyledString();
    EXPECT_EQ(low["error_code"].asString(),"fee_below_minimum") << low.toStyledString();
    EXPECT_FALSE(low["retryable"].asBool());
    const auto required=low["required_fee_una"].asUInt64();
    const auto vsize=low["virtual_size"].asUInt64();
    // Three actions, no transparent inputs: priced at 1 una/vB, about 12.4k.
    EXPECT_EQ(required,vsize);EXPECT_GT(required,12000u);EXPECT_LT(required,13000u);
    nothing_left("after the default fee");
    const auto [short_request,short_by_one]=spend(0xa8,required-1);(void)short_request;
    EXPECT_EQ(short_by_one["error_code"].asString(),"fee_below_minimum") << short_by_one.toStyledString();
    EXPECT_EQ(short_by_one["required_fee_una"].asUInt64(),required);
    nothing_left("after one una short");

    // The exact fee the refusal named is admitted, at exactly the size it gave.
    const auto [request,queued]=spend(0xa9,required);
    ASSERT_FALSE(queued.isMember("error")) << queued.toStyledString();
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        orchard::Hash op{};op[0]=0xa9;
        ASSERT_EQ(ServiceProofTerminal(use->OrchardProofs(),op),wallet::OrchardProofJobs::State::Succeeded);
    }
    din::Json finish;finish["account"]=Json::UInt64(3);finish["request_id"]=request["request_id"];
    const auto finished=rpc_context_wallet_orchard_finishspend(ctx,finish);
    ASSERT_FALSE(finished.isMember("error")) << finished.toStyledString();
    ASSERT_TRUE(finished["admitted"].asBool()) << finished.toStyledString();
    const auto pooled=f.f.ingress->mempool().getMempoolEntry(uint256::FromHexUnsafe(finished["txid"].asString()));
    ASSERT_TRUE(bool(pooled));
    EXPECT_EQ(pooled->tx.GetVirtualSize(),vsize) << "queue-time size differs from the admitted transaction";
    EXPECT_EQ(pooled->tx.ExplicitFee(),std::optional<uint64_t>(required));
    ASSERT_TRUE(f.Mine(pooled->tx));
    deliver();
    // The payment went to the account's own address: only the fee left it.
    EXPECT_EQ(f.Account().account.Scan().BalanceUna(),450000u-required);
    f.context.tx_ingress=nullptr;
}

} // namespace dinero
