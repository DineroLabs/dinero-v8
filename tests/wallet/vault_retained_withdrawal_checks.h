#pragma once
// Patched actual vault-state/wallet/RPC paths with synthetic funded coins and
// explicit component ingress. No network or synchronization-removal controls.
class VaultRetainedWithdrawal : public WalletBatchRpc {
protected:
    dinero::vault::VaultStateDomain domain;
    dinero::vault::VaultServiceConfig config;
    dinero::vault::WithdrawalPaymentTerms terms{1,10000,"explicit retained-withdrawal fixture"};
    dinero::vault::BoundVaultService vault;
    std::unique_ptr<dinero::vault::WalletWithdrawalDispatchOwner> dispatch_owner;
    auto dispatcher(const dinero::WalletSigningIdentity& identity) {
        dispatch_owner=std::make_unique<dinero::vault::WalletWithdrawalDispatchOwner>(ctx,service,identity,domain);
        return dispatch_owner->Factory();
    }
    std::array<uint8_t,32> block{};
    dinero::vault::AccountId account{"vault-owner"};
    dinero::WalletSigningIdentity selected() {
        return dinero::CaptureWalletSigningIdentity(service->get(),"owner");
    }
    auto capture() {
        return [hash=block](uint64_t height,const std::vector<dinero::vault::VaultDepositQuery>& queries) {
            dinero::vault::VaultTipSnapshot out{height,hash,{}};
            for(const auto& q:queries)out.deposits.push_back({q,hash,true});return out;
        };
    }
    auto backend() {return std::make_unique<dinero::vault::InMemorySigningBackend>(dinero::vault::BackendId{"bound-wallet-fixture"});}
    void SetUp() override {
        dinero::SelectParams(dinero::Chain::REGTEST);
        WalletBatchRpc::SetUp();if(HasFatalFailure())return;
        domain.network=static_cast<uint8_t>(dinero::GetActiveChain());
        dinero::uint256 genesis;ASSERT_TRUE(dinero::uint256::FromHex(dinero::Params().genesis_hash,genesis));
        std::copy(genesis.begin(),genesis.end(),domain.genesis.begin());block.fill(73);
        config.confirmation_policy.k_observe=1;config.confirmation_policy.k_credit=1;config.confirmation_policy.k_settle=1;
        const auto identity=selected();
        vault=dinero::vault::WalletVaultStateOwner::CreateNewService(service,identity.session,domain,config,
            backend(),[](uint64_t){return std::array<uint8_t,32>{};},[](const auto&,uint64_t,const auto&){return false;},capture(),
            dispatcher(identity));
        std::array<uint8_t,32> deposit{};deposit.fill(31);
        vault.service->recordDeposit(deposit,7,account,100000,20,block);vault.service->tipChanged(20);
        ingress->test=[&](const dinero::Transaction& tx){before_preflight(tx);return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());};
    }
    void TearDown() override {dispatch_owner.reset();vault.service.reset();WalletBatchRpc::TearDown();}
    void reopen() {
        const auto id=vault.identity;vault.service.reset();auto& w=service->get();w.open("owner");w.unlockWallet("historical-rpc",0);
        const auto identity=selected();
        vault=dinero::vault::WalletVaultStateOwner::OpenExistingService(service,identity.session,domain,id,
            backend(),[](uint64_t){return std::array<uint8_t,32>{};},[](const auto&,uint64_t,const auto&){return false;},capture(),
            dispatcher(identity));
    }
    auto enqueue(uint64_t amount=20000) {return vault.service->enqueueWithdrawal(account,amount,modern.spk,terms);}
    void submission(const dinero::Transaction& tx,const dinero::vault::WithdrawalId& id) {
        auto& w=service->get();EXPECT_EQ(sqlite3_get_autocommit(w.getCurrentDatabase()),1);
        const auto records=w.getPendingPayments();ASSERT_EQ(records.size(),1u);const auto& row=records[0];
        ASSERT_TRUE(row.intent.request);EXPECT_EQ(row.intent.request->owner,vault.identity);EXPECT_EQ(row.intent.request->id,id);
        EXPECT_EQ(row.intent.request->audit_context,terms.audit_context);EXPECT_EQ(row.signed_body,tx.Serialize(dinero::TxSerializationMode::WithWitness));
        EXPECT_TRUE(row.intent.additional_recipients.empty());EXPECT_EQ(row.intent.amount_una,20000u);
        // Taking the real seed/SQL owner here demonstrates that dispatch holds
        // neither of those owners. This callback does not invoke chain work.
        auto owner=dinero::vault::VaultStateTransaction::OpenExisting(w,selected().session,domain,vault.identity);
        const auto& requests=owner->Current().state.withdrawals;ASSERT_EQ(requests.size(),1u);
        EXPECT_TRUE(std::holds_alternative<dinero::vault::WithdrawalSigning>(requests[0].state));owner->Commit();
        verify(tx,{hd});
    }
    dinero::vault::WithdrawalPaymentRetained retained(const dinero::vault::WithdrawalId& id) {
        const auto state=vault.service->withdrawalState(id);
        if(!std::holds_alternative<dinero::vault::WithdrawalPaymentRetained>(state))throw std::runtime_error("fixture expected retained state");
        const auto saved=std::get<dinero::vault::WithdrawalPaymentRetained>(state);const auto record=service->get().getPendingPayments().at(0);
        dinero::Transaction tx;EXPECT_TRUE(dinero::TransactionSerializer::Deserialize(tx,record.signed_body));
        EXPECT_LT(saved.vout,tx.vout.size());EXPECT_EQ(tx.vout.at(saved.vout).scriptPubKey,modern.spk);EXPECT_EQ(tx.vout.at(saved.vout).GetValue(),20000u);
        std::array<uint8_t,32> digest{};SHA256(record.signed_body.data(),record.signed_body.size(),digest.data());EXPECT_EQ(digest,saved.body_sha256);
        EXPECT_EQ(saved.fee_una,record.fee_una);EXPECT_EQ(vault.service->accountLocked(account),20000u);
        EXPECT_TRUE(service->get().isUTXOLocked(hd.GetTxIdHex(),hd.vout));return saved;
    }
};
TEST_F(VaultRetainedWithdrawal, AcceptedPaymentRetainsExactBodyAndTermsAcrossReopen) {
    const auto id=enqueue();ingress->submit=[&](const dinero::Transaction& tx){submission(tx,id);return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());};
    EXPECT_EQ(vault.service->processNextWithdrawal(),std::optional<dinero::vault::WithdrawalId>{id});
    const auto saved=retained(id);const auto bytes=dinero::vault::EncodeVaultState(vault.service->captureState());
    EXPECT_EQ(bytes[5],'2');const auto decoded=dinero::vault::DecodeVaultState(bytes);
    ASSERT_EQ(decoded.withdrawals.size(),1u);ASSERT_TRUE(decoded.withdrawals[0].request.payment_terms);
    EXPECT_EQ(*decoded.withdrawals[0].request.payment_terms,terms);EXPECT_NO_THROW(dinero::vault::ReplayVaultStateLedger(decoded));
    EXPECT_THROW(vault.service->markWithdrawalIncluded(id,21),std::runtime_error);
    EXPECT_EQ(dinero::vault::EncodeVaultState(vault.service->captureState()),bytes);
    EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,1);reopen();EXPECT_EQ(retained(id),saved);
    EXPECT_FALSE(vault.service->processNextWithdrawal());EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,1);
}
TEST_F(VaultRetainedWithdrawal, RejectedSubmissionIsRetainedWithoutBroadcastClaim) {
    const auto id=enqueue();ingress->submit=[&](const dinero::Transaction& tx){submission(tx,id);return dinero::TxAcceptResult::Rejected(dinero::TxRejectCode::INSUFFICIENT_FEE,"fixture admission changed");};
    EXPECT_EQ(vault.service->processNextWithdrawal(),std::optional<dinero::vault::WithdrawalId>{id});
    const auto saved=retained(id);EXPECT_EQ(ingress->submits,1);reopen();EXPECT_EQ(retained(id),saved);
    EXPECT_FALSE(vault.service->processNextWithdrawal());EXPECT_EQ(ingress->submits,1);
}
TEST_F(VaultRetainedWithdrawal, WalletCommitSurvivesVaultWriteRefusalAndResolvesOnce) {
    const auto id=enqueue();ingress->submit=[&](const dinero::Transaction& tx)->dinero::TxAcceptResult {
        submission(tx,id);
        sqlite3_set_authorizer(service->get().getCurrentDatabase(),[](void*,int action,const char* table,const char*,const char*,const char*) {
            return action==SQLITE_UPDATE && table && std::strcmp(table,"wallet_vault_states")==0?SQLITE_DENY:SQLITE_OK;
        },nullptr);
        throw std::runtime_error("fixture outcome unavailable");
    };
    EXPECT_THROW(vault.service->processNextWithdrawal(),std::exception);sqlite3_set_authorizer(service->get().getCurrentDatabase(),nullptr,nullptr);
    EXPECT_TRUE(std::holds_alternative<dinero::vault::WithdrawalSigning>(vault.service->withdrawalState(id)));
    const auto body=service->get().getPendingPayments().at(0).signed_body;EXPECT_EQ(ingress->submits,1);reopen();
    EXPECT_EQ(vault.service->processNextWithdrawal(),std::optional<dinero::vault::WithdrawalId>{id});
    (void)retained(id);EXPECT_EQ(service->get().getPendingPayments().at(0).signed_body,body);
    EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,1);
}
TEST_F(VaultRetainedWithdrawal, MissingBodyKeepsSigningAcrossReopenWithoutRegeneration) {
    const auto id=enqueue();daemon.tx_ingress=nullptr;
    EXPECT_THROW(vault.service->processNextWithdrawal(),std::runtime_error);
    EXPECT_TRUE(std::holds_alternative<dinero::vault::WithdrawalSigning>(vault.service->withdrawalState(id)));
    EXPECT_TRUE(service->get().getPendingPayments().empty());reopen();daemon.tx_ingress=ingress.get();
    const auto saved=dinero::vault::EncodeVaultState(vault.service->captureState());
    EXPECT_THROW(vault.service->processNextWithdrawal(),std::runtime_error);
    EXPECT_EQ(dinero::vault::EncodeVaultState(vault.service->captureState()),saved);
    EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);
}
TEST_F(VaultRetainedWithdrawal, ExplicitTermsAndCollectiveReservationsRefuseBeforeEffects) {
    auto bad=terms;bad.maximum_fee_una=0;const auto initial=dinero::vault::EncodeVaultState(vault.service->captureState());
    EXPECT_THROW(vault.service->enqueueWithdrawal(account,20000,modern.spk,bad),std::runtime_error);
    bad=terms;bad.audit_context=std::string("a\0b",3);EXPECT_THROW(vault.service->enqueueWithdrawal(account,20000,modern.spk,bad),std::runtime_error);
    EXPECT_EQ(dinero::vault::EncodeVaultState(vault.service->captureState()),initial);
    (void)enqueue(60000);const auto reserved=dinero::vault::EncodeVaultState(vault.service->captureState());
    EXPECT_THROW(enqueue(40001),std::runtime_error);EXPECT_EQ(dinero::vault::EncodeVaultState(vault.service->captureState()),reserved);
    EXPECT_NO_THROW(enqueue(40000));EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);
}
TEST_F(VaultRetainedWithdrawal, BoundWalletSessionAndServiceCannotRetargetDispatch) {
    const auto identity=selected();din::Json p,recipients(Json::arrayValue),recipient,binding;
    recipient["address"]=modern_address;recipient["amount_una"]=din::Json::UInt64(20000);recipients.append(recipient);
    binding["domain"]="vault_withdrawal";binding["owner"]=std::string(64,'4');binding["id"]=std::string(32,'7');
    binding["fee_rate_hint"]=din::Json::UInt64(1);binding["maximum_fee_una"]=din::Json::UInt64(10000);binding["audit_context"]="fixture";
    p["recipients"]=recipients;p["request"]=binding;
    service->get().open("owner");service->get().unlockWallet("historical-rpc",0);
    EXPECT_TRUE(dinero::DispatchBoundWalletRequest(ctx,p,service,identity).isMember("error"));
    const auto current=selected();EXPECT_TRUE(dinero::DispatchBoundWalletRequest(ctx,p,{},current).isMember("error"));
    auto wrong=current;wrong.name="other";EXPECT_TRUE(dinero::DispatchBoundWalletRequest(ctx,p,service,wrong).isMember("error"));
    EXPECT_TRUE(dinero::DispatchBoundWalletRequest(ctx,request(),service,current).isMember("error"));
    EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);
}

TEST_F(VaultRetainedWithdrawal, RetainedRecipientAtNonzeroOutputNeverUsesAssumedZero) {
    const auto id=enqueue();auto input=unsigned_tx({hd});input.tx.vout.clear();
    // Actual signed wallet owner has change first and recipient second.
    dinero::TxOutput change;change.value=dinero::AmountUna::Una(79000);change.scriptPubKey=modern.spk;input.tx.vout.push_back(change);
    dinero::TxOutput recipient;recipient.value=dinero::AmountUna::Una(20000);recipient.scriptPubKey=modern.spk;input.tx.vout.push_back(recipient);
    input.change_amount=79000;input.change_address=modern_address;
    dinero::PendingPaymentIntent intent;intent.address=modern_address;intent.amount_una=20000;
    dinero::PendingPaymentRequest binding;binding.owner=vault.identity;binding.id=id;
    binding.fee_rate_hint=terms.fee_rate_hint;binding.maximum_fee_una=terms.maximum_fee_una;binding.audit_context=terms.audit_context;intent.request=binding;
    const auto payment=dinero::SignAndStageWalletPayment(service->get(),selected(),input,intent);
    ASSERT_TRUE(payment.success)<<payment.error;const auto body=service->get().getPendingPayments().at(0).signed_body;
    EXPECT_EQ(vault.service->processNextWithdrawal(),std::optional<dinero::vault::WithdrawalId>{id});
    EXPECT_EQ(retained(id).vout,1u);EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);
    const auto snapshot=vault.service->captureState();
    const auto* initiated=std::get_if<dinero::vault::WithdrawalInitiated>(&snapshot.entries.back());ASSERT_NE(initiated,nullptr);EXPECT_EQ(initiated->request.vout,1u);
    reopen();EXPECT_EQ(retained(id).vout,1u);EXPECT_EQ(service->get().getPendingPayments().at(0).signed_body,body);
}
TEST_F(VaultRetainedWithdrawal, HistoricalSnapshotAndMissingTermsRemainExplicitlyUnspecified) {
    // Literal DNVS01 framing fixture: one pending alice/200 request, no terms
    // field in that format, no deposit/ledger owner or historical fee claim.
    std::vector<uint8_t> bytes;ASSERT_TRUE(util::unhex("444e5653303101000000000000008813000000000000881300000000000088130000000000000100000000000000010000000000000001000000000000000000000000000000881300000000000088130000000000000200000000000000010000000000000000000000000000000000000000000000000100000000000000010000000000000000000000000000000500000000000000616c696365c800000000000000010000000000000051080000000000000001",bytes));
    auto decoded=dinero::vault::DecodeVaultState(bytes);ASSERT_EQ(decoded.withdrawals.size(),1u);
    EXPECT_FALSE(decoded.withdrawals[0].request.payment_terms);EXPECT_EQ(decoded.withdrawals[0].request.amount,200u);
    EXPECT_EQ(decoded.withdrawals[0].request.account.raw,"alice");
    EXPECT_FALSE(dinero::vault::DecodeVaultState(dinero::vault::EncodeVaultState(decoded)).withdrawals[0].request.payment_terms);
    const auto id=vault.service->enqueueWithdrawal(account,20000,modern.spk);
    const auto original=dinero::vault::EncodeVaultState(vault.service->captureState());
    EXPECT_THROW(vault.service->processNextWithdrawal(),std::runtime_error);
    EXPECT_TRUE(std::holds_alternative<dinero::vault::WithdrawalPending>(vault.service->withdrawalState(id)));
    EXPECT_EQ(dinero::vault::EncodeVaultState(vault.service->captureState()),original);reopen();
    EXPECT_THROW(vault.service->processNextWithdrawal(),std::runtime_error);
    EXPECT_EQ(dinero::vault::EncodeVaultState(vault.service->captureState()),original);
    EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);EXPECT_TRUE(service->get().getPendingPayments().empty());
}
