// Patched-path request retention tests. Synthetic wallet inputs only; no
// unsafe-original, synchronization-removal or external broadcast controls.
class WalletRequestOwner : public WalletBatchPayment {
protected:
    std::string format() {
        auto& w=service->get();auto lease=w.AcquireDatabaseLease();auto pin=lease->CopyRecoverySeed(lease->Session());
        struct Secrets {
            std::vector<uint8_t> material,plain;std::array<uint8_t,32> key{};
            ~Secrets() {OPENSSL_cleanse(material.data(),material.size());OPENSSL_cleanse(plain.data(),plain.size());OPENSSL_cleanse(key.data(),key.size());}
        } secrets;
        const std::string domain="Dinero wallet pending payment owner v1";
        secrets.material.assign(domain.begin(),domain.end());
        secrets.material.insert(secrets.material.end(),pin->Bytes().begin(),pin->Bytes().end());
        ::SHA256(secrets.material.data(),secrets.material.size(),secrets.key.data());
        std::vector<uint8_t> sealed;
        if(!util::unhex(envelope(lease->Database()),sealed) || sealed.size()<28)throw std::runtime_error("fixture envelope framing");
        const std::vector<uint8_t> nonce(sealed.begin(),sealed.begin()+12),cipher(sealed.begin()+12,sealed.end());
        secrets.plain=dinero::crypto::decryptAesGcm(cipher,secrets.key,nonce);
        if(secrets.plain.size()<6)throw std::runtime_error("fixture envelope plaintext");
        return {secrets.plain.begin(),secrets.plain.begin()+6};
    }
    dinero::PendingPaymentIntent request() {
        dinero::PendingPaymentIntent intent{modern_address,199000,""};
        dinero::PendingPaymentRequest request;
        request.owner[0]=0x42;request.id[0]=0x17;
        request.fee_rate_hint=2;request.maximum_fee_una=1000;request.audit_context="vault synthetic request";
        intent.request=request;return intent;
    }
    dinero::WalletSigningIdentity identity() {
        return dinero::CaptureWalletSigningIdentity(service->get(),"owner");
    }
    std::optional<dinero::PendingPayment> find(const dinero::PendingPaymentIntent& intent) {
        return dinero::FindRetainedWalletPayment(service->get(),identity(),intent);
    }
    dinero::UnsignedTransaction shifted(uint32_t offset) {
        auto input=payment();
        for(size_t i=0;i<input.selected_utxos.size();++i) {
            input.selected_utxos[i].vout+=offset;input.tx.vin[i].prevout.vout+=offset;
            fund(input.selected_utxos[i]);
        }
        return input;
    }
};
TEST_F(WalletRequestOwner, MixedOriginsResolveExactBodyAndReopen) {
    auto& w=service->get();auto* db=w.getCurrentDatabase();
    const auto first=shifted(0);ASSERT_TRUE(stage(first).success);EXPECT_EQ(format(),"DNPP01");
    const auto original=w.getPendingPayments().at(0);EXPECT_FALSE(original.intent.request);
    const auto old_envelope=envelope(db);EXPECT_FALSE(find(request()));EXPECT_EQ(envelope(db),old_envelope);
    const auto batch_request=batch_intent();const auto batch_input=batch(batch_request,7);
    for(const auto& c:batch_input.selected_utxos)fund(c);
    ASSERT_TRUE(retain(batch_input,batch_request).success);EXPECT_EQ(format(),"DNPP02");
    const auto old_batch=w.getPendingPayments();ASSERT_EQ(old_batch.size(),2u);
    EXPECT_EQ(old_batch[0].signed_body,original.signed_body);EXPECT_FALSE(old_batch[1].intent.request);
    const auto intent=request();const auto input=shifted(14);
    ASSERT_FALSE(find(intent));const auto signed_payment=retain(input,intent);
    ASSERT_TRUE(signed_payment.success)<<signed_payment.error;verify(signed_payment.signed_tx.tx,input.selected_utxos);EXPECT_EQ(format(),"DNPP03");
    const auto saved=envelope(db);const auto changes=sqlite3_total_changes(db);
    const auto retained=find(intent);ASSERT_TRUE(retained);EXPECT_EQ(retained->intent,intent);
    EXPECT_EQ(retained->signed_body,signed_payment.signed_tx.tx.Serialize(dinero::TxSerializationMode::WithWitness));
    EXPECT_EQ(retained->fee_una,1000u);EXPECT_EQ(sqlite3_total_changes(db),changes);EXPECT_EQ(envelope(db),saved);
    const auto records=w.getPendingPayments();ASSERT_EQ(records.size(),3u);
    for(size_t i=0;i<2;++i) {
        EXPECT_EQ(records[i].intent,old_batch[i].intent);EXPECT_EQ(records[i].signed_body,old_batch[i].signed_body);
        EXPECT_EQ(records[i].created_at,old_batch[i].created_at);EXPECT_EQ(records[i].fee_una,old_batch[i].fee_una);
    }
    EXPECT_EQ(count(db,"transactions"),3);EXPECT_EQ(w.getLockedUTXOs().size(),6u);
    const auto listing=rpc_context_wallet_listpendingpayments(ctx,din::Json());
    ASSERT_FALSE(listing.isMember("error"))<<listing.toStyledString();ASSERT_EQ(listing["payments"].size(),3u);
    EXPECT_FALSE(listing["payments"][0].isMember("request"));EXPECT_FALSE(listing["payments"][1].isMember("request"));
    const auto& row=listing["payments"][2];EXPECT_EQ(row["hex"].asString(),util::hex(retained->signed_body));
    const auto& binding=row["request"];EXPECT_EQ(binding["domain"].asString(),"vault_withdrawal");
    EXPECT_EQ(binding["owner"].asString(),util::hex(std::vector<uint8_t>(intent.request->owner.begin(),intent.request->owner.end())));
    EXPECT_EQ(binding["id"].asString(),util::hex(std::vector<uint8_t>(intent.request->id.begin(),intent.request->id.end())));
    EXPECT_EQ(binding["fee_rate_hint"].asUInt64(),2u);EXPECT_EQ(binding["maximum_fee_una"].asUInt64(),1000u);
    EXPECT_EQ(binding["audit_context"].asString(),intent.request->audit_context);EXPECT_EQ(envelope(db),saved);
    SigningHook hook{w};sqlite3_trace_v2(db,SQLITE_TRACE_STMT,SigningHook::trace,&hook);
    const auto duplicate=retain(input,intent);sqlite3_trace_v2(db,0,nullptr,nullptr);
    EXPECT_FALSE(duplicate.success);EXPECT_TRUE(duplicate.signed_tx.tx.vin.empty());EXPECT_FALSE(hook.fired);
    EXPECT_EQ(envelope(db),saved);EXPECT_EQ(count(db,"transactions"),3);
    w.open("owner");w.unlockWallet("historical-rpc",0);
    const auto reopened=find(intent);ASSERT_TRUE(reopened);EXPECT_EQ(reopened->signed_body,retained->signed_body);
    EXPECT_EQ(reopened->intent,intent);EXPECT_EQ(reopened->created_at,retained->created_at);
    EXPECT_EQ(envelope(w.getCurrentDatabase()),saved);EXPECT_EQ(w.getLockedUTXOs().size(),6u);
}
TEST_F(WalletRequestOwner, ExactPayloadMismatchPreservesRetainedRequest) {
    auto& w=service->get();auto* db=w.getCurrentDatabase();const auto input=shifted(0);const auto intent=request();
    ASSERT_TRUE(retain(input,intent).success);const auto saved=envelope(db);const auto recorded=find(intent);ASSERT_TRUE(recorded);
    std::vector<dinero::PendingPaymentIntent> changed;
    auto value=intent;value.address=w.getNewAddress();changed.push_back(value);
    value=intent;++value.amount_una;changed.push_back(value);
    value=intent;value.label="another label";changed.push_back(value);
    value=intent;value.additional_recipients={{modern_address,1}};changed.push_back(value);
    value=intent;++value.request->fee_rate_hint;changed.push_back(value);
    value=intent;++value.request->maximum_fee_una;changed.push_back(value);
    value=intent;value.request->audit_context="other audit context";changed.push_back(value);
    for(const auto& mismatch:changed) {
        EXPECT_THROW(find(mismatch),std::runtime_error);const auto result=retain(input,mismatch);
        EXPECT_FALSE(result.success);EXPECT_TRUE(result.signed_tx.tx.vin.empty());EXPECT_EQ(envelope(db),saved);
    }
    value=intent;value.request->owner[1]=1;EXPECT_FALSE(find(value));
    value=intent;value.request->id[1]=1;EXPECT_FALSE(find(value));
    EXPECT_EQ(envelope(db),saved);EXPECT_EQ(count(db,"transactions"),1);
    EXPECT_EQ(find(intent)->signed_body,recorded->signed_body);EXPECT_EQ(w.getLockedUTXOs().size(),2u);
}
TEST_F(WalletRequestOwner, CheckedWriteCommitAndReadRefusalsPreserveOwner) {
    auto& w=service->get();auto* db=w.getCurrentDatabase();const auto input=shifted(0);const auto intent=request();
    const auto absent=[&] {EXPECT_TRUE(w.getPendingPayments().empty());EXPECT_EQ(count(db,"transactions"),0);EXPECT_FALSE(find(intent));EXPECT_TRUE(w.getLockedUTXOs().empty());};
    sql(db,"CREATE TRIGGER request_history_failure BEFORE INSERT ON transactions BEGIN SELECT RAISE(ABORT,'request fixture'); END");
    auto result=retain(input,intent);EXPECT_FALSE(result.success);EXPECT_TRUE(result.signed_tx.tx.vin.empty());absent();
    sql(db,"DROP TRIGGER request_history_failure");
    struct Commit {bool writing=false;int commits=0;
        static int trace(unsigned,void* raw,void* stmt,void*) {auto& h=*static_cast<Commit*>(raw);const auto* s=sqlite3_sql(static_cast<sqlite3_stmt*>(stmt));if(s && std::strstr(s,"UPDATE wallet_meta SET pending_payment_owner"))h.writing=true;return 0;}
        static int hook(void* raw) {auto& h=*static_cast<Commit*>(raw);if(h.writing){++h.commits;return 1;}return 0;}
    } commit;
    sqlite3_trace_v2(db,SQLITE_TRACE_STMT,Commit::trace,&commit);sqlite3_commit_hook(db,Commit::hook,&commit);
    result=retain(input,intent);sqlite3_commit_hook(db,nullptr,nullptr);sqlite3_trace_v2(db,0,nullptr,nullptr);
    EXPECT_FALSE(result.success);EXPECT_TRUE(result.signed_tx.tx.vin.empty());EXPECT_EQ(commit.commits,1);absent();
    ASSERT_TRUE(retain(input,intent).success);const auto saved=envelope(db);const auto selected=identity();
    sql(db,"BEGIN");EXPECT_THROW(dinero::FindRetainedWalletPayment(w,selected,intent),std::runtime_error);
    EXPECT_FALSE(sqlite3_get_autocommit(db));sql(db,"ROLLBACK");
    sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char* column,const char*,const char*) {
        return action==SQLITE_READ && table && column && std::strcmp(table,"wallet_meta")==0 && std::strcmp(column,"pending_payment_owner")==0?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    EXPECT_THROW(find(intent),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);
    struct Interrupt {sqlite3* db;bool fired=false;
        static int trace(unsigned kind,void* raw,void* stmt,void*) {auto& h=*static_cast<Interrupt*>(raw);const auto* s=sqlite3_sql(static_cast<sqlite3_stmt*>(stmt));if(kind==SQLITE_TRACE_ROW && !h.fired && s && std::strstr(s,"SELECT pending_payment_owner")){h.fired=true;sqlite3_interrupt(h.db);}return 0;}
    } interrupted{db};
    sqlite3_trace_v2(db,SQLITE_TRACE_ROW,Interrupt::trace,&interrupted);EXPECT_THROW(find(intent),std::runtime_error);sqlite3_trace_v2(db,0,nullptr,nullptr);
    EXPECT_TRUE(interrupted.fired);EXPECT_TRUE(find(intent));EXPECT_EQ(envelope(db),saved);EXPECT_EQ(count(db,"transactions"),1);
}
TEST_F(WalletRequestOwner, InvalidIdentityFeeAndStaleSessionRefuse) {
    auto& w=service->get();auto* db=w.getCurrentDatabase();const auto input=shifted(0);const auto intent=request();
    std::vector<dinero::PendingPaymentIntent> invalid;
    auto value=intent;value.request.reset();invalid.push_back(value);
    value=intent;value.request->owner={};invalid.push_back(value);
    value=intent;value.request->id={};invalid.push_back(value);
    value=intent;value.request->domain=static_cast<dinero::PendingPaymentRequestDomain>(99);invalid.push_back(value);
    value=intent;value.request->audit_context=std::string("a\0b",3);invalid.push_back(value);
    for(const auto& malformed:invalid) {
        EXPECT_THROW(find(malformed),std::runtime_error);
        if(malformed.request) {const auto result=retain(input,malformed);EXPECT_FALSE(result.success);EXPECT_TRUE(result.signed_tx.tx.vin.empty());}
        EXPECT_EQ(count(db,"transactions"),0);EXPECT_TRUE(w.getPendingPayments().empty());
    }
    value=intent;value.request->maximum_fee_una=999;
    const auto over=retain(input,value);EXPECT_FALSE(over.success);EXPECT_TRUE(over.signed_tx.tx.vin.empty());EXPECT_FALSE(find(value));EXPECT_TRUE(w.getPendingPayments().empty());
    ASSERT_TRUE(retain(input,intent).success);const auto saved=envelope(db);const auto selected=identity();
    const auto retained=find(intent);ASSERT_TRUE(retained);
    ASSERT_TRUE(w.confirmTransaction(retained->txid,2));EXPECT_EQ(find(intent)->signed_body,retained->signed_body);
    w.lockWallet();EXPECT_THROW(dinero::FindRetainedWalletPayment(w,selected,intent),std::runtime_error);
    w.open("owner");w.unlockWallet("historical-rpc",0);
    EXPECT_THROW(dinero::FindRetainedWalletPayment(w,selected,intent),std::runtime_error);
    EXPECT_EQ(find(intent)->signed_body,retained->signed_body);EXPECT_EQ(envelope(w.getCurrentDatabase()),saved);
    const auto current=identity();w.open("other");EXPECT_THROW(dinero::FindRetainedWalletPayment(w,current,intent),std::runtime_error);
}
