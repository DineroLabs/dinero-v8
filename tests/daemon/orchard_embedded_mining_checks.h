#pragma once
#if defined(DINERO_TEST_ORCHARD_ORIGIN) && defined(DINERO_TEST_EMBEDDED_MINING)
namespace {
void RunEmbeddedOrchardMining(solo::MinerBackend backend,bool historical_start=false) {
    // The private fixture activates at 102, with genuine PoW enforced. No
    // public chain parameter or running node is changed by this test.
    RawIngressFixture raw(true);
    struct AcceptorContext {
        DaemonContext* previous;
        AcceptorContext(DaemonContext& context,DaemonContext* old):previous(old) {
            BlockAcceptor::SetContext(&context);
        }
        ~AcceptorContext(){BlockAcceptor::SetContext(previous);}
    } acceptor(raw.context,raw.previous);
    // Historical ingress uses these normal DaemonApp-owned dependencies too.
    raw.context.block_storage=raw.files;
    const uint32_t activation=historical_start?103:102;
    if(historical_start) {
        MutableParams().orchard_activation_height=activation;
        if(MutableParams().orchard_catalog_commitment_activation_height!=UINT32_MAX)
            MutableParams().orchard_catalog_commitment_activation_height=activation;
        ShieldedStateStartupTestAccess::InstallHistoricalMiningValidator(*raw.f.service);
    }
    struct Notices final : RuntimeBlockNotifications {
        std::atomic<unsigned> published{0};
        uint32_t activation;
        explicit Notices(uint32_t height):activation(height){}
        struct Prepared final : PreparedRuntimeBlockNotifications {
            Notices& owner;explicit Prepared(Notices& value):owner(value){}
            void PublishAfterCommit() noexcept override {++owner.published;}
        };
        std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(
            const RuntimeBlockBody& body,uint32_t height,RuntimeBlockDirection) override {
            if(body.IsOrchardProfile()!=(height>=activation) || height<102)
                throw std::runtime_error("wrong embedded mining profile");
            return std::make_unique<Prepared>(*this);
        }
    };
    const auto notices=std::make_shared<Notices>(activation);
    raw.f.service->setRuntimeBlockNotifications(notices);
    if(!historical_start)ASSERT_TRUE(raw.f.ingress->SubmitBody(raw.f.Shield(),TxOrigin::INTERNAL).accepted());
    const auto auth=std::make_shared<RpcAuth>(raw.f.path.string());ASSERT_TRUE(auth->generate_cookie());
    HttpRpcServer server("127.0.0.1",39723);
    ExecutionContext execution;execution.daemon=&raw.context;
    server.set_auth(auth);server.set_daemon_context(&raw.context);
    server.register_method("getblockchaininfo",[&](const auto& p){return ::rpc_context_getblockchaininfo(execution,p);});
    server.register_method("getblockhash",[&](const auto& p){return ::rpc_context_getblockhash(execution,p);});
    std::atomic<unsigned> fetched{0};std::atomic<bool> delayed{false},submitted{false};
    const auto wait_for=[&](const auto& ready){
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!ready.load() && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    server.register_method("mining.getjob",[&](const auto& p){
        const auto call=fetched.fetch_add(1);
        auto result=::rpc_mining_getjob(execution,p);
        if(historical_start && call==1){delayed=true;wait_for(submitted);}
        return result;
    });
    server.register_method("mining.submit",[&](const auto& p){
        if(historical_start && !submitted.load())wait_for(delayed);
        auto result=::rpc_mining_submit(execution,p);submitted=true;return result;
    });
    server.start(); // A port collision fails before any client is created.
    solo::SoloMiner miner;
    std::mutex errors_mutex;std::string errors;
    miner.setErrorCallback([&](const auto& error){std::lock_guard lock(errors_mutex);errors+=error+"\n";});
    solo::MinerConfig config;config.rpc_url="http://127.0.0.1:39723";
    config.cookie_path=auth->get_cookie_path();config.payout_address=OrchardMiningPayout;
    config.backend=backend;config.threads=2;config.gpu_batch_size=64;
    const bool started=miner.start(config);
    if(started) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(45);
        while(miner.getStats().blocks_accepted<(historical_start?3u:2u) && miner.getStats().blocks_found<6 &&
              std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        miner.stop();
    }
    server.stop();
    ASSERT_TRUE(started)<<errors;
    const auto stats=miner.getStats();
    ASSERT_GE(stats.blocks_accepted,historical_start?3u:2u)<<errors;
    EXPECT_EQ(stats.blocks_rejected,0u)<<errors;
    ASSERT_GE(notices->published.load(),2u);
    const auto tip=raw.f.db.getTip();ASSERT_TRUE(tip.ok());ASSERT_GE(tip->height,int(activation+1));
    for(uint32_t height=102;height<=uint32_t(tip->height);++height) {
        const auto hash=raw.f.db.getBlockHashByHeight(height);ASSERT_TRUE(hash.ok());
        const auto stored=ReadRuntimeBlockUnderLock(raw.f.db,raw.files.get(),*hash,height);
        ASSERT_TRUE(stored.ok());
        EXPECT_EQ(stored->IsOrchardProfile(),height>=activation);
        if(height<activation)continue;
        const auto& body=stored->Orchard();std::string error;
        EXPECT_TRUE(body.CheckIdentityCommitments(true,error))<<error;
        EXPECT_TRUE(body.CheckCoinbaseHeight(height,error))<<error;
        const auto target=uint256::FromHexUnsafe(DineroPoW::BitsToTargetHex(body.Header().difficulty));
        EXPECT_FALSE(target<body.Header().GetHash());
        if(height==102 && !historical_start)EXPECT_GT(body.Transactions().size(),1u); // Genuine shield proof/body.
    }
}
}
TEST(OrchardEmbeddedMining, CpuCrossesActivationAndMinesSuccessorThroughRealHttp) {
    RunEmbeddedOrchardMining(solo::MinerBackend::Cpu);
}
TEST(OrchardEmbeddedMining, CpuMinesHistoricalThenCrossesActivationAndSuccessor) {
    RunEmbeddedOrchardMining(solo::MinerBackend::Cpu,true);
}
#ifdef DINERO_TEST_EMBEDDED_METAL
TEST(OrchardEmbeddedMining, MetalCrossesActivationAndMinesSuccessorThroughRealHttp) {
    RunEmbeddedOrchardMining(solo::MinerBackend::Metal);
}
#endif
#endif
