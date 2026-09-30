#pragma once
namespace dinero {
namespace {
struct VaultRpcInputFixture {
    VaultRuntimeReset reset;
    ExecutionContext context;
    std::shared_ptr<vault::VaultService> service;
    std::vector<uint8_t> script;
    VaultRpcInputFixture():script(34,7) {
        script[0]=0x51;script[1]=32;
        vault::InitializeVaultRuntime(VaultOwnerConfig());service=vault::GetVaultRuntimeService();
        std::array<uint8_t,32> txid{},hash{};txid.fill(7);hash.fill(1);
        service->recordDeposit(txid,0,vault::AccountId{"rpc-owner"},1000,100,hash);
        service->tipChanged(120);
    }
    din::Json Withdraw() {
        din::Json object;object["account_id"]="rpc-owner";object["amount_una"]=din::Json::UInt64(50);
        object["destination_script_pub_key"]=util::hex(script);auto p=din::arr();p.append(object);return p;
    }
};
}
TEST(VaultRpcInput, TypedWithdrawalArgumentsPreserveState) {
    VaultRpcInputFixture f;ASSERT_EQ(f.service->accountSpendable(vault::AccountId{"rpc-owner"}),1000u);
    const auto valid=f.Withdraw();const auto first=din::rpc_vault_withdraw(f.context,valid);
    ASSERT_FALSE(first.isMember("error"))<<first.toStyledString();EXPECT_EQ(first["status"].asString(),"pending");
    EXPECT_EQ(f.service->withdrawalQueueDepth(),1);
    const auto before=f.service->metrics();const auto entries=f.service->entriesSince(0);
    for(int mode=0;mode<13;++mode) {
        auto p=valid;
        if(mode==0)p[0]["amount_una"]=50.0;
        if(mode==1)p[0]["amount_una"]="50";
        if(mode==2)p[0]["amount_una"]=true;
        if(mode==3)p[0]["amount_una"]=-1;
        if(mode==4)p[0]["amount_una"]=din::Json::UInt64(0);
        if(mode==5)p[0].removeMember("amount_una");
        if(mode==6)p[0]["account_id"]=7;
        if(mode==7)p[0]["account_id"]="";
        if(mode==8)p[0]["account_id"]=std::string("a\0b",3);
        if(mode==9)p[0]["destination_address"]=17;
        if(mode==10)p[0]["destination_script_pub_key"]=true;
        if(mode==11)p.append(valid[0]);
        if(mode==12)p=valid[0];
        din::Json refused;ASSERT_NO_THROW(refused=din::rpc_vault_withdraw(f.context,p))<<mode;
        EXPECT_TRUE(refused.isMember("error"))<<mode;EXPECT_FALSE(refused.isMember("request_id"));
        EXPECT_EQ(f.service->metrics(),before);EXPECT_EQ(f.service->entriesSince(0),entries);
    }
    // A typed address retains the documented precedence over a script field.
    auto address=valid;address[0]["destination_address"]=VaultOwnerAddress(f.script);
    address[0]["destination_script_pub_key"]="ignored by address precedence";
    ASSERT_FALSE(din::rpc_vault_withdraw(f.context,address).isMember("error"));
    EXPECT_EQ(f.service->withdrawalQueueDepth(),2);
}
TEST(VaultRpcInput, ExactHexIdentifiersAndStatus) {
    VaultRpcInputFixture f;const auto valid=f.Withdraw();const auto accepted=din::rpc_vault_withdraw(f.context,valid);
    ASSERT_FALSE(accepted.isMember("error"));auto status=din::arr();status.append(accepted["request_id"]);
    ASSERT_EQ(din::rpc_vault_withdrawal_status(f.context,status)["state"].asString(),"pending");
    auto upper=status[0].asString();for(char& c:upper)if(c>='a'&&c<='f')c=static_cast<char>(c-'a'+'A');
    status[0]=upper;EXPECT_EQ(din::rpc_vault_withdrawal_status(f.context,status)["state"].asString(),"pending");
    const auto before=f.service->metrics();const auto entries=f.service->entriesSince(0);
    for(const auto& hex:std::vector<std::string>{"0z","+1"," 1","-1",std::string("0\0",2),"a"}) {
        auto p=valid;p[0]["destination_script_pub_key"]=hex;
        din::Json refused;ASSERT_NO_THROW(refused=din::rpc_vault_withdraw(f.context,p));EXPECT_TRUE(refused.isMember("error"));
        auto id=status;id[0]=hex+std::string(30,'0');ASSERT_NO_THROW(refused=din::rpc_vault_withdrawal_status(f.context,id));EXPECT_TRUE(refused.isMember("error"));
        din::Json obj;obj["txid"]=hex+std::string(62,'0');obj["vout"]=0;obj["account_id"]="rpc-owner";
        auto observe=din::arr();observe.append(obj);ASSERT_NO_THROW(refused=din::rpc_vault_observe(f.context,observe));
        EXPECT_EQ(refused["error"]["message"].asString(),"invalid txid hex");
        EXPECT_EQ(f.service->metrics(),before);EXPECT_EQ(f.service->entriesSince(0),entries);
    }
    for(int mode=0;mode<7;++mode) {
        din::Json obj;obj["txid"]=std::string(64,'1');obj["vout"]=0;obj["account_id"]="rpc-owner";
        if(mode==0)obj["vout"]=0.0;if(mode==1)obj["vout"]="0";if(mode==2)obj["vout"]=true;
        if(mode==3)obj["vout"]=-1;if(mode==4)obj["vout"]=din::Json::UInt64(uint64_t{1}<<32);
        if(mode==5)obj.removeMember("vout");if(mode==6)obj["txid"]=1;
        auto p=din::arr();p.append(obj);din::Json refused;ASSERT_NO_THROW(refused=din::rpc_vault_observe(f.context,p));
        EXPECT_EQ(refused["error"]["message"].asString(),"txid/account_id must be strings and vout a uint32 integer");
        EXPECT_EQ(f.service->metrics(),before);EXPECT_EQ(f.service->entriesSince(0),entries);
    }
}
TEST(VaultRpcInput, ExplicitOperatorBindingInput) {
    VaultRpcInputFixture f;din::Json object;object["address"]=VaultOwnerAddress(f.script);object["account"]="rpc-owner";
    auto valid=din::arr();valid.append(object);ASSERT_FALSE(din::rpc_vault_setoperator(f.context,valid).isMember("error"));
    const auto before=vault::GetVaultOperator();
    for(int mode=0;mode<7;++mode) {
        auto p=valid;
        if(mode==0)p[0].removeMember("address");if(mode==1)p[0]["address"]=0;if(mode==2)p[0]["address"]=false;
        if(mode==3)p[0]["account"]=8;if(mode==4)p[0]["account"]=std::string("a\0b",3);
        if(mode==5)p.append(valid[0]);if(mode==6)p=valid[0];
        din::Json refused;ASSERT_NO_THROW(refused=din::rpc_vault_setoperator(f.context,p));EXPECT_TRUE(refused.isMember("error"))<<mode;
        EXPECT_EQ(vault::GetVaultOperator().address,before.address);EXPECT_EQ(vault::GetVaultOperator().account,before.account);
    }
    auto read=din::arr();read.append("rpc-owner");EXPECT_EQ(din::rpc_vault_account_spendable(f.context,read)["spendable_una"].asUInt64(),1000u);
    EXPECT_EQ(din::rpc_vault_account_metrics(f.context,read)["confirmed_una"].asUInt64(),1000u);
    read[0]=true;EXPECT_TRUE(din::rpc_vault_account_spendable(f.context,read).isMember("error"));
    EXPECT_TRUE(din::rpc_vault_account_metrics(f.context,read).isMember("error"));
    auto disabled=valid;disabled[0]["address"]="";const auto result=din::rpc_vault_setoperator(f.context,disabled);
    ASSERT_FALSE(result.isMember("error"));EXPECT_EQ(result["status"].asString(),"disabled");EXPECT_TRUE(vault::GetVaultOperator().address.empty());
}
} // namespace dinero
