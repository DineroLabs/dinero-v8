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

// v8.2.7 cancel: a queued (Reserved, unsigned) operation held its notes and
// blocked the desktop's Orchard page with no way out. cancelreserved removes
// it and releases the notes, frees its finished proof, and refuses an unknown
// or signed operation without changing anything.
TEST(WalletV3NativeOrchardSpend, CancelReservedReleasesQueuedSpendAndRefusesSigned) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-orchard-cancel-index");
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

    constexpr uint64_t fee=20000;
    const auto id_hex=[](uint8_t tag){std::vector<uint8_t> id(32);id[0]=tag;return util::hex(id);};
    const auto spend=[&](uint8_t tag){
        din::Json request,pay;
        request["account"]=Json::UInt64(3);
        request["expected_revision"]=Json::UInt64(f.Account().revision);
        request["request_id"]=id_hex(tag);
        request["fee_una"]=Json::UInt64(fee);
        request["payments"]=din::arr();request["outputs"]=din::arr();
        pay["address"]=created["address"];pay["amount_una"]=Json::UInt64(400000);
        request["payments"].append(pay);
        return rpc_context_wallet_orchard_queuespend(ctx,request);
    };
    const auto cancel=[&](const std::string& id){
        din::Json request;request["account"]=Json::UInt64(3);request["operation_id"]=id;
        return rpc_context_wallet_orchard_cancelreserved(ctx,request);
    };
    const auto reserved=[&]{
        return rpc_context_wallet_orchard_getbalance(ctx,OrchardOperationListRequest())["reserved_confirmed_una"].asUInt64();
    };
    const auto listed=[&]{
        return rpc_context_wallet_orchard_listoperations(ctx,OrchardOperationListRequest())["operations"];
    };
    const auto job=[&](uint8_t tag){
        auto use=WalletService::AcquireWalletUse(f.wallet);
        orchard::Hash op{};op[0]=tag;return use->OrchardProofs().Query(op);
    };

    // A queued spend holds all three notes, and its proof finishes.
    const auto queued=spend(0xc1);
    ASSERT_FALSE(queued.isMember("error")) << queued.toStyledString();
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        orchard::Hash op{};op[0]=0xc1;
        ASSERT_EQ(ServiceProofTerminal(use->OrchardProofs(),op),wallet::OrchardProofJobs::State::Succeeded);
    }
    ASSERT_EQ(reserved(),450000u);
    ASSERT_EQ(listed().size(),1u);
    ASSERT_EQ(listed()[0]["durable_state"].asString(),"reserved");

    // An unknown ID is refused and changes nothing.
    const auto revision_before=f.Account().revision;
    const auto unknown=cancel(id_hex(0xc9));
    EXPECT_EQ(unknown["error_code"].asString(),"operation_not_found") << unknown.toStyledString();
    EXPECT_EQ(f.Account().revision,revision_before);
    EXPECT_EQ(reserved(),450000u);

    // Cancelling the queued spend releases its notes and frees its proof slot.
    const auto cancelled=cancel(listed()[0]["operation_id"].asString());
    ASSERT_FALSE(cancelled.isMember("error")) << cancelled.toStyledString();
    EXPECT_TRUE(cancelled["cancelled"].asBool());
    EXPECT_EQ(cancelled["account"].asUInt64(),3u);
    EXPECT_EQ(cancelled["operation_id"].asString(),id_hex(0xc1));
    EXPECT_EQ(cancelled["account_revision"].asUInt64(),f.Account().revision);
    EXPECT_EQ(f.Account().revision,revision_before+1);
    EXPECT_TRUE(listed().empty()) << listed().toStyledString();
    EXPECT_EQ(reserved(),0u);
    EXPECT_FALSE(job(0xc1).has_value()) << "the finished proof of a cancelled operation still holds a slot";
    din::Json stale;stale["account"]=Json::UInt64(3);stale["request_id"]=id_hex(0xc1);
    const auto refinished=rpc_context_wallet_orchard_finishspend(ctx,stale);
    EXPECT_TRUE(refinished.isMember("error")) << "a cancelled operation was finished: " << refinished.toStyledString();
    EXPECT_TRUE(listed().empty());

    // The released notes pay a new request, which is signed and admitted.
    const auto again=spend(0xc2);
    ASSERT_FALSE(again.isMember("error")) << again.toStyledString();
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        orchard::Hash op{};op[0]=0xc2;
        ASSERT_EQ(ServiceProofTerminal(use->OrchardProofs(),op),wallet::OrchardProofJobs::State::Succeeded);
    }
    din::Json finish;finish["account"]=Json::UInt64(3);finish["request_id"]=id_hex(0xc2);
    const auto finished=rpc_context_wallet_orchard_finishspend(ctx,finish);
    ASSERT_FALSE(finished.isMember("error")) << finished.toStyledString();
    ASSERT_TRUE(finished["admitted"].asBool()) << finished.toStyledString();

    // A signed operation cannot be cancelled here, and stays as it was.
    const auto signed_revision=f.Account().revision;
    const auto refused=cancel(id_hex(0xc2));
    EXPECT_EQ(refused["error_code"].asString(),"operation_signed") << refused.toStyledString();
    EXPECT_FALSE(refused["retryable"].asBool());
    EXPECT_EQ(f.Account().revision,signed_revision);
    ASSERT_EQ(listed().size(),1u);
    EXPECT_EQ(listed()[0]["durable_state"].asString(),"signed");
    EXPECT_EQ(reserved(),450000u);

    const auto pooled=f.f.ingress->mempool().getMempoolEntry(uint256::FromHexUnsafe(finished["txid"].asString()));
    ASSERT_TRUE(bool(pooled));
    ASSERT_TRUE(f.Mine(pooled->tx));
    deliver();
    EXPECT_EQ(f.Account().account.Scan().BalanceUna(),450000u-fee);
    f.context.tx_ingress=nullptr;
}

// The owner's stuck row is a queued shield. A shield's transparent inputs and
// change are held only by its entry: while it is queued no other shield can
// use the coin; once cancelled, a new shield takes the same coin and confirms.
TEST(WalletV3NativeOrchardSpend, CancelReservedShieldReleasesItsTransparentCoin) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-cancel-shield-index");
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
    const auto id_hex=[](uint8_t tag){std::vector<uint8_t> id(32);id[0]=tag;return util::hex(id);};
    const auto proved=[&](uint8_t tag){
        auto use=WalletService::AcquireWalletUse(f.wallet);
        orchard::Hash op{};op[0]=tag;return ServiceProofTerminal(use->OrchardProofs(),op);
    };
    // One transparent coin of 100000: unshield to the wallet's own address.
    {
        din::Json request,out;
        request["account"]=Json::UInt64(3);
        request["expected_revision"]=Json::UInt64(f.Account().revision);
        request["request_id"]=id_hex(0xd0);request["fee_una"]=Json::UInt64(10000);
        request["payments"]=din::arr();request["outputs"]=din::arr();
        out["address"]=own_address;out["amount_una"]=Json::UInt64(100000);request["outputs"].append(out);
        ASSERT_FALSE(rpc_context_wallet_orchard_queuespend(ctx,request).isMember("error"));
        ASSERT_EQ(proved(0xd0),wallet::OrchardProofJobs::State::Succeeded);
        din::Json finish;finish["account"]=Json::UInt64(3);finish["request_id"]=request["request_id"];
        const auto finished=rpc_context_wallet_orchard_finishspend(ctx,finish);
        ASSERT_TRUE(finished["admitted"].asBool()) << finished.toStyledString();
        const auto pooled=f.f.ingress->mempool().getMempoolEntry(uint256::FromHexUnsafe(finished["txid"].asString()));
        ASSERT_TRUE(bool(pooled));ASSERT_TRUE(f.Mine(pooled->tx));
        deliver();
    }
    const auto orchard_before=f.Account().account.Scan().BalanceUna();
    const auto shield=[&](uint8_t tag){
        din::Json request,pay;
        request["account"]=Json::UInt64(3);
        request["expected_revision"]=Json::UInt64(f.Account().revision);
        request["request_id"]=id_hex(tag);request["fee_una"]=Json::UInt64(10000);
        request["payments"]=din::arr();
        pay["address"]=created["address"];pay["amount_una"]=Json::UInt64(20000);request["payments"].append(pay);
        return std::pair{request,rpc_context_wallet_orchard_queueshield(ctx,request)};
    };
    // Confirmed operations stay listed (the unshield above): look at the queued ones.
    const auto listed=[&]{
        din::Json queued=din::arr();
        const auto all=rpc_context_wallet_orchard_listoperations(ctx,OrchardOperationListRequest());
        for(const auto& op:all["operations"])
            if(op["durable_state"].asString()=="reserved")queued.append(op);
        return queued;
    };

    const auto [first_request,first]=shield(0xd1);(void)first_request;
    ASSERT_FALSE(first.isMember("error")) << first.toStyledString();
    ASSERT_EQ(listed().size(),1u);
    EXPECT_EQ(listed()[0]["durable_state"].asString(),"reserved");
    EXPECT_EQ(listed()[0]["completion_method"].asString(),"wallet.orchard.finishshield");
    // The queued shield holds the only coin.
    const auto [blocked_request,blocked]=shield(0xd2);(void)blocked_request;
    EXPECT_TRUE(blocked.isMember("error")) << "a second shield used the held coin: " << blocked.toStyledString();
    ASSERT_EQ(listed().size(),1u);

    din::Json cancel;cancel["account"]=Json::UInt64(3);cancel["operation_id"]=id_hex(0xd1);
    const auto cancelled=rpc_context_wallet_orchard_cancelreserved(ctx,cancel);
    ASSERT_FALSE(cancelled.isMember("error")) << cancelled.toStyledString();
    EXPECT_TRUE(cancelled["cancelled"].asBool());
    EXPECT_TRUE(listed().empty()) << listed().toStyledString();

    // The released coin pays a new shield, which is admitted and confirmed.
    const auto [request,queued]=shield(0xd3);
    ASSERT_FALSE(queued.isMember("error")) << "the cancelled shield still holds its coin: " << queued.toStyledString();
    ASSERT_EQ(proved(0xd3),wallet::OrchardProofJobs::State::Succeeded);
    const auto finished=rpc_context_wallet_orchard_finishshield(ctx,request);
    ASSERT_FALSE(finished.isMember("error")) << finished.toStyledString();
    ASSERT_TRUE(finished["admitted"].asBool()) << finished.toStyledString();
    const auto pooled=f.f.ingress->mempool().getMempoolEntry(uint256::FromHexUnsafe(finished["txid"].asString()));
    ASSERT_TRUE(bool(pooled));ASSERT_TRUE(f.Mine(pooled->tx));
    deliver();
    EXPECT_EQ(f.Account().account.Scan().BalanceUna(),orchard_before+20000u);
    f.context.tx_ingress=nullptr;
}

// v8.2.7: the fee is checked again just before an operation is signed. A
// request queued without the queue-time check (before the upgrade, or with a
// lower floor) and finished below the floor would otherwise become a signed
// operation that admission refuses on every retry and nothing can cancel.
// Refused before signing, it stays queued; cancel then releases its notes.
TEST(WalletV3NativeOrchardSpend, FinishBelowFeeFloorRefusedBeforeSigningAndStaysCancellable) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-finish-fee-floor-index");
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
    auto& pool=f.f.ingress->mempool();
    ASSERT_EQ(pool.getMinFeeRate(),1.0);
    std::vector<uint8_t> id(32);id[0]=0xe1;
    din::Json request,pay;
    request["account"]=Json::UInt64(3);
    request["expected_revision"]=Json::UInt64(f.Account().revision);
    request["request_id"]=util::hex(id);request["fee_una"]=Json::UInt64(10000);
    request["payments"]=din::arr();request["outputs"]=din::arr();
    pay["address"]=created["address"];pay["amount_una"]=Json::UInt64(400000);request["payments"].append(pay);
    // Queued as a pre-upgrade wallet would have: no floor at queue time.
    pool.setMinFeeRate(0.0);
    const auto queued=rpc_context_wallet_orchard_queuespend(ctx,request);
    pool.setMinFeeRate(1.0);
    ASSERT_FALSE(queued.isMember("error")) << queued.toStyledString();
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        orchard::Hash op{};op[0]=0xe1;
        ASSERT_EQ(ServiceProofTerminal(use->OrchardProofs(),op),wallet::OrchardProofJobs::State::Succeeded);
    }
    const auto listed=[&]{
        return rpc_context_wallet_orchard_listoperations(ctx,OrchardOperationListRequest())["operations"];
    };
    din::Json finish;finish["account"]=Json::UInt64(3);finish["request_id"]=request["request_id"];
    for(int attempt=0;attempt<2;++attempt){
        SCOPED_TRACE(attempt);
        const auto refused=rpc_context_wallet_orchard_finishspend(ctx,finish);
        ASSERT_TRUE(refused.isMember("error")) << "signed below the floor: " << refused.toStyledString();
        EXPECT_EQ(refused["error_code"].asString(),"fee_below_minimum") << refused.toStyledString();
        EXPECT_GT(refused["required_fee_una"].asUInt64(),12000u);
        EXPECT_EQ(refused["required_fee_una"].asUInt64(),refused["virtual_size"].asUInt64());
        ASSERT_EQ(listed().size(),1u);
        EXPECT_EQ(listed()[0]["durable_state"].asString(),"reserved") << listed().toStyledString();
        EXPECT_FALSE(listed()[0].isMember("txid"));
    }
    EXPECT_EQ(pool.size(),0u);
    din::Json cancel;cancel["account"]=Json::UInt64(3);cancel["operation_id"]=util::hex(id);
    const auto cancelled=rpc_context_wallet_orchard_cancelreserved(ctx,cancel);
    ASSERT_FALSE(cancelled.isMember("error")) << cancelled.toStyledString();
    EXPECT_TRUE(listed().empty());
    EXPECT_EQ(rpc_context_wallet_orchard_getbalance(ctx,OrchardOperationListRequest())["reserved_confirmed_una"].asUInt64(),0u);
    f.context.tx_ingress=nullptr;
}

// The owner's case: shields have no queue-time fee check, so a queued shield
// can carry any fee. Finishing one below the floor is refused before it is
// signed, with the fee it needs; it stays queued and can be cancelled.
TEST(WalletV3NativeOrchardSpend, FinishShieldBelowFeeFloorRefusedBeforeSigningAndStaysCancellable) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-finish-shield-fee-floor-index");
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
    const auto id_hex=[](uint8_t tag){std::vector<uint8_t> id(32);id[0]=tag;return util::hex(id);};
    const auto proved=[&](uint8_t tag){
        auto use=WalletService::AcquireWalletUse(f.wallet);
        orchard::Hash op{};op[0]=tag;return ServiceProofTerminal(use->OrchardProofs(),op);
    };
    {
        din::Json request,out;
        request["account"]=Json::UInt64(3);
        request["expected_revision"]=Json::UInt64(f.Account().revision);
        request["request_id"]=id_hex(0xe0);request["fee_una"]=Json::UInt64(10000);
        request["payments"]=din::arr();request["outputs"]=din::arr();
        out["address"]=own_address;out["amount_una"]=Json::UInt64(100000);request["outputs"].append(out);
        ASSERT_FALSE(rpc_context_wallet_orchard_queuespend(ctx,request).isMember("error"));
        ASSERT_EQ(proved(0xe0),wallet::OrchardProofJobs::State::Succeeded);
        din::Json finish;finish["account"]=Json::UInt64(3);finish["request_id"]=request["request_id"];
        const auto finished=rpc_context_wallet_orchard_finishspend(ctx,finish);
        ASSERT_TRUE(finished["admitted"].asBool()) << finished.toStyledString();
        const auto pooled=f.f.ingress->mempool().getMempoolEntry(uint256::FromHexUnsafe(finished["txid"].asString()));
        ASSERT_TRUE(bool(pooled));ASSERT_TRUE(f.Mine(pooled->tx));
        deliver();
    }
    const auto queued_ops=[&]{
        din::Json queued=din::arr();
        const auto all=rpc_context_wallet_orchard_listoperations(ctx,OrchardOperationListRequest());
        for(const auto& op:all["operations"])if(op["durable_state"].asString()!="signed"||!op.isMember("chain_observation")||
            op["chain_observation"].isNull())queued.append(op);
        return queued;
    };
    din::Json request,pay;
    request["account"]=Json::UInt64(3);
    request["expected_revision"]=Json::UInt64(f.Account().revision);
    request["request_id"]=id_hex(0xe2);request["fee_una"]=Json::UInt64(5000); // below 1 una/vB for any shield
    request["payments"]=din::arr();
    pay["address"]=created["address"];pay["amount_una"]=Json::UInt64(20000);request["payments"].append(pay);
    const auto queued=rpc_context_wallet_orchard_queueshield(ctx,request);
    ASSERT_FALSE(queued.isMember("error")) << queued.toStyledString();
    ASSERT_EQ(proved(0xe2),wallet::OrchardProofJobs::State::Succeeded);
    din::Json stored;stored["account"]=Json::UInt64(3);stored["request_id"]=id_hex(0xe2);
    for(const auto& finish:{request,stored}){
        const auto refused=rpc_context_wallet_orchard_finishshield(ctx,finish);
        ASSERT_TRUE(refused.isMember("error")) << "signed below the floor: " << refused.toStyledString();
        EXPECT_EQ(refused["error_code"].asString(),"fee_below_minimum") << refused.toStyledString();
        EXPECT_EQ(refused["required_fee_una"].asUInt64(),refused["virtual_size"].asUInt64());
        EXPECT_GT(refused["required_fee_una"].asUInt64(),5000u);
        const auto open=queued_ops();
        ASSERT_EQ(open.size(),1u) << open.toStyledString();
        EXPECT_EQ(open[0]["durable_state"].asString(),"reserved") << open.toStyledString();
    }
    EXPECT_EQ(f.f.ingress->mempool().size(),0u);
    din::Json cancel;cancel["account"]=Json::UInt64(3);cancel["operation_id"]=id_hex(0xe2);
    const auto cancelled=rpc_context_wallet_orchard_cancelreserved(ctx,cancel);
    ASSERT_FALSE(cancelled.isMember("error")) << cancelled.toStyledString();
    EXPECT_TRUE(queued_ops().empty()) << queued_ops().toStyledString();
    f.context.tx_ingress=nullptr;
}

} // namespace dinero
