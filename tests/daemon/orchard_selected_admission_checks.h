#pragma once
// Included by the actual selected-history/service fixture inside namespace dinero.
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
class OrchardAdmissionFixture {
public:
    enum class HistoricalSpend { None, MatureCoin, SameBlockChild };
    HistoricalSpend historical_spend=HistoricalSpend::None;
    ChainParams previous_params = Params();
    bool previous_stateless = GetConfig().utreexo_stateless;
    std::filesystem::path path;
    std::shared_ptr<ChainDB> database=std::make_shared<ChainDB>();
    ChainDB& db=*database;
    std::shared_ptr<ChainstateService> service=std::make_shared<ChainstateService>();
    std::shared_ptr<MempoolService> ingress;
    std::vector<uint8_t> script;
    orchard::Hash secret{};
    std::vector<uint8_t> last_bundle;
    CBlockIndex tip;
    std::vector<Block> blocks;
    std::unique_ptr<assumeutxo::AssumeUtxoReplayEngine> replay;
    const ChainWriteToken token = ChainWriteToken::CreateForTesting();
    static void Require(bool value,const char* file=__builtin_FILE(),int line=__builtin_LINE()) {
        if (!value) throw std::runtime_error(std::string("selected parent fixture requirement at ")+file+":"+std::to_string(line));
    }
    struct AdmissionLogger final:ILogger {
        bool refuse=false;
        void info(const std::string& message)override {
            if(refuse && message.find("Added Orchard transaction")!=std::string::npos)
                throw std::runtime_error("fixture before-publication refusal");
        }
        void log(LogLevel,const std::string& message)override{info(message);}
        void debug(const std::string&)override{} void warning(const std::string&)override{}
        void error(const std::string&)override{} void setLogLevel(LogLevel)override{}
        void setLogFile(const std::string&)override{} void shutdown()override{}
    };
    std::shared_ptr<AdmissionLogger> logger=std::make_shared<AdmissionLogger>();
    struct AdmissionNotifications final:RuntimeBlockNotifications {
        std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(const RuntimeBlockBody&,uint32_t,RuntimeBlockDirection)override {
            throw std::logic_error("Admission must not publish a block notification");
        }
    };
    static BlockHeader SolveHeader(BlockHeader header) {
        for(uint32_t nonce=0;nonce<4'000'000;++nonce) {
            header.nonce=nonce;
            if(consensus::CheckProofOfWork(header,false))return header;
        }
        throw std::runtime_error("isolated admission fixture nonce search exhausted");
    }
    static BlockHeader SolveHistoricalHeader(BlockHeader header,uint32_t height) {
        // Offline-mined hints for the exact deterministic historical fixture.
        // The complete solved header must match, and real PoW is checked on
        // every use. Changed bodies/profiles fall back to the original search.
        struct Vector { uint32_t nonce; const char* hash; };
        static constexpr Vector vectors[] = {
            {0u, "78a9a4cd903449a933e37d2eee495521f89210532898f1363bf37c953d16f301"},
            {100315u, "0000add6e176c32e774617b52e8af23ed64615da08163507748075a0d646ee0c"},
            {108146u, "0000285e8bee65d046e959d330d6c5d9f810890df8bcfdf3a3d371934b3daa84"},
            {30634u, "0000906419793ec0819dc53904b8153a5d6f9f7e5dac0aa6dd0805f78c047b91"},
            {28009u, "00007171a768d49b9833be0d6d0fceb7dd35f42a063284db058a52e8c5269e06"},
            {64742u, "0000a2a8a30da21b2d4b4a369f73d27366bfbc0747bdefb72b3706fa02f161f4"},
            {6850u, "0000f94daf3dc5fecd301cfd881dcaee165d533804c8c2c35d543f200b7558cf"},
            {36615u, "000023cd9e097a69835e5d5fed0e964953f91b0fbae1d99fc474a73155f269b9"},
            {45597u, "0000c4f20d4cb04f45c5e975394b378c46995923106aecee3e26ce1ffc4aec23"},
            {125140u, "0000c69bdc48dbbab77d5c33f3ba7effd15a0f33928e08e5bd0a36efff06d642"},
            {43019u, "0000173a4b53ecc9d863e6902f758f7c179e977c2297f653087dcc48ee798b46"},
            {49712u, "00009e352aa9a39644ca3e3175236fd50e207bd159b35f5e258e0770f0abb260"},
            {103651u, "0000d6dae9f23cc7196fae04b4f40c384f78529abf5ee1602242be99164ec900"},
            {25366u, "0000c4953de1e3541d75f93f9c3f95656fe1d22dce9c0e0f22ab771b9c558b64"},
            {76930u, "00008b52e39b9a081504e2a1740649e53b49b76c8bd364c90134206d155262c2"},
            {59932u, "00005d846da8ff5154d237fe9850fb2debc9b8246b8f5d0abc7f5e71e81f4020"},
            {4534u, "00001dcbf9e2847439c526c5c585a958a9ea870448c33100be5d018388c4ee80"},
            {14840u, "0000a843a4f086676bb9eef03a04b672c703dfc1f628da6fa341b0cfbbf284c0"},
            {120242u, "00003a38ad4f9baafbaee7670ee8b6928a80b30aa84c581e18df317ff9ea112c"},
            {107779u, "00009ad2285017b6580fc49d9d597b18441cc5569352579b9244edca84b789b1"},
            {126682u, "0000d3c515e790373a0ea4f271d53feb22a1996d2f96f3d1a84c675f6361943b"},
            {16131u, "0000b3392d8175fd3e7f78bd65bedfff19e4e9fe7cdf7124b23c97f543542987"},
            {25702u, "00003c3e2b93d307b03bd8b74362538e83026f2519b10d423ce03faff9a1292b"},
            {215357u, "000040fe5fa4a91d20e54cc25f9c404411918f62ad282e8a3d97c05099874136"},
            {73267u, "000041fb0f5af2a9cd64971e6e18cdf2d86248eba63ec27ce1dbda1e37884b2d"},
            {131696u, "00002c17a90e243f006b940677151b1cc6369e8a313162ae272d3a8b0763f971"},
            {55166u, "0000a23fb4092cf29d256f53a5ee72f957196c66048bb0f451e666db31544197"},
            {37371u, "00008b5d392d14cb94926c8aff2ff5ccdf453582dd9d19493157b88d79d944d4"},
            {341630u, "0000f559e9dadb776fd2f4d11f67b005457c699e1df6b11a724ded261bf62cc4"},
            {180388u, "0000b81fbfa7543828067a5fff3f6030814faecab2d27e9c53346db32c4a61f8"},
            {4525u, "0000ee3d6fc397cbcf076ae0afdb2dc1f726f185d37c93db191c285a3b83b967"},
            {74240u, "00001a233013f70d694e75e5ac2f182b611d288a8eea4edc82bb3347b068e6d9"},
            {23823u, "0000ab57b247048695576843e894d24cc429a5ff464aac5a67e1a1cd8dd77111"},
            {40757u, "00003632de421d0673f28ef4f73623a3ce1d5bb053985685aa28bc5d12155351"},
            {44490u, "00004a931ac68c7ab7be67649f4f505a0f2203702ae1977963a12c64dea433bf"},
            {128142u, "0000a5d2c5e732558ee27273d2f5c53e9f7c671feae9f209a573a9a8bc5e454b"},
            {29041u, "0000115abe9853fccb59bf0f213f5f672d5d8acd2f453823e7d9a6f08e1daf60"},
            {5345u, "00007c6f6688b1e81b4cecc6141571286d785cd4129138c152a4cf064817b43b"},
            {96303u, "000013b5a4fc9c8f43214fb1d41c086c398a8ebb3b3f3f5ed77ae20acdd0bbf0"},
            {2259u, "0000be39c192fff2fae7875be6b251ca274fb3a89888a8f65111a8374c033c35"},
            {202559u, "0000e26785b7698c0218c7675049cfbed55d5966fe48071ff1899cbb35470eae"},
            {134344u, "000003e6780d9eeb4f45cda440ff654d72eb5937dfda3ff83c8f1350f4b61ada"},
            {61159u, "00004142e3c70b19598464e0467e7c2cadd18cd216eb9f6c2f766cdbe5d00e2a"},
            {102196u, "0000d2ebbecb9e052b6413737305445f892a25454e69f004939764f10aa93edf"},
            {4485u, "000051f5e582e53db38736cb65d80626eaa9c10138e1c5052c29b88a285a5152"},
            {20069u, "00000edeaac71c59015d6b4ee3e22f991b7efd081b9f50b803cac34ce5afbce5"},
            {19394u, "000076bcd3e0cb6258ae381e1dc081309791994f65ee2d5765c4305ddb660fa5"},
            {1426u, "00009f81c9fc115701ab7e1a58a45ca314a80e3cdd55d3078e4e95a3c5ccf13f"},
            {4545u, "00000942e0b07f58c8376eeb52e730fd194e0d80c193be33b898c52b99245bc5"},
            {147503u, "0000d4f0587942835a6667c1e3688844ea650d00bde3ac90e6c2c792ea7fe393"},
            {27279u, "0000c20844c0ea73321e364b4787cb8f20b1b28361d6b5b5a58bd826832bad5d"},
            {91174u, "00006923c63867f18f6290f03f307e111a5181cf7a92a6127298271ea4f64215"},
            {885u, "0000e9227daecea05bdae66c9d95a3a598010bf652354291d3d15a1f3e92d45f"},
            {88443u, "00008e407d7361af48217bd6bbab43af3b0bd65bbeb896bf6267eeb91172348a"},
            {144643u, "000016c4bf21f2098aa0a9066cff905b38ce0a144498f314aefc176c93a7a54a"},
            {2904u, "00003f36702cb5788b78118af333859ad006e5bfe4c379f8d4d3c968db897e14"},
            {58023u, "0000191a815448102d55ffb1c4ea49c47365d6ff44fb925a6f5db6ec86e1c04f"},
            {92166u, "0000920514cefb1f58d5849e0a2f4af68a92af8392b493c9b6be419831012b32"},
            {53848u, "0000b7f660a6aee3987a0bc3fa9995c07bd99dcde9d776762091bffed4638101"},
            {22918u, "0000f62024555ca91ad1f4bf4430a16d95a040e37c3b8a923f2220bfe5606aed"},
            {91043u, "00009799aff8691be66fa6858a611f28e6a4db04ac70a3354901dc3e092bfc79"},
            {17206u, "0000d91cdf073e0a618d1542d89943f6f60625bbb3bdb0c6ed56f4982849235a"},
            {18076u, "0000777ba85867d92b1afc261a6ee3c08d06b1041bbe88d0103f3bcd55264fef"},
            {72444u, "00006fec9c803bb5b2fd7fb673ee1b956fed9d53cc78213ab2fbadfbfc1628c5"},
            {101477u, "00001dff637d6a2c20e8b9c85d04df07465d65c7568e5b8cda8959c3bea22d76"},
            {2297u, "00009fa6603982e63d02ddcc2bd4fd079b9c2592e0d52487d4809de0bcb5a620"},
            {87158u, "0000e30c0ef876780277dca4e4b9c63ed1dd93368f0a8765ae9818dc59368148"},
            {18213u, "00003f796070ff9c93770863bf14b1d36ead2d0ed97f25d76b8206f1c19fe226"},
            {210885u, "000035743b7749f3c085c6d4551739f34da990158690cd1f883daa4efd6f7333"},
            {72934u, "00003da31fddc59fe6fc3b33ae30e5b6816abde1db2dfa19c59a7248a132bddf"},
            {83450u, "00000b2e2f7ba7e32283ba4e3e8a18e8eec7d66e5175fa9a5fa14a79dbc0f470"},
            {70040u, "00007e93673653b59e08021b632a3f7edad80c3e6c16f37d790d220329df0218"},
            {63000u, "0000a449b3b4f56a57249b1c101e31d796687d22f3b85a70e21fa916dda249dc"},
            {381324u, "0000e5026a84eee71378508c44970edb38fc01ba72f830c4ff95b408e7ecbe7c"},
            {67478u, "000097657f4cc391b7b255f645667af1ec42ef4e27aba999254bb81915993df0"},
            {18460u, "00009744c600a3dedf2945baf7be0f0ef51e1ab0d712dd5a10f49da48b2e1f58"},
            {14802u, "000051883b30a2075d1833cc337fbb02f13ac4968d3a8b1cd4c5bcb337dc851e"},
            {207470u, "00003870e7a1a1071c7886f395ddc3b8bf660e89175df7f22348b9167496a080"},
            {81335u, "0000a5061b3b9c6649f67625ad2ba53bffeb6c2d6d9a7065e7754c958d38a1a3"},
            {17625u, "0000f6be84d253a79768089d4ec826b491643951ed767064404653794b12f36e"},
            {2684u, "00001fec68a31de8664c4bb3614e52b0e294aec09312d4f86c34aae54589d791"},
            {45446u, "00007a05d72365b094975c7f93b403246653fb56f80deb8828060eae770616f6"},
            {31204u, "0000c30d50970ffab49ae7e6eb6db323953f0f3ffb06eb695744934c04a4c6a4"},
            {100993u, "00002d7f5f8abad50294754ec57b3979026269e5c51919429fb1828245066481"},
            {83806u, "00004e181c6143803fceb9fad9b4656d949994b3ebf2bff3db4edeeddc60913e"},
            {101558u, "0000ab4228b61a4a06a1ab51bd8bfbbf9de980d5230e1157153e279c3eaf96fb"},
            {86413u, "0000adb9dc4b63aa7b42eea261a9ae82fccf9f9e0e2cd5ebdcdeaf38bf9f2bef"},
            {22432u, "0000f7813c820f682c0bdd89bd1fda6596c5f599b56e68f3ce497fd3d3ec489f"},
            {198690u, "00001aff788f3844ebca683102713cb2c816a6b51d00c19709167e5af5261d94"},
            {57902u, "00005d98bc873078381e75da895fb876e12861a14529012ccb9b39ca52c6b05d"},
            {7705u, "00007bc3c2a9f47796429ada42ea37be85a2539faad8542554a39a8d0a6f5aac"},
            {47160u, "00009f98d0291f68d7efb6b42931728a70097b36a4d59042d6ebfdc35a058c7a"},
            {136928u, "0000835d24d6a90fd6a6265d13b1eeee9508247e1adcc860128efa20d0d55e3a"},
            {68332u, "000034eb314a0666f0dbde7f8fdc9eea8c2955060c15ee1cae0d06f7b0b2dc23"},
            {54798u, "0000afd4e777675ce92394e34e285df3c938ea1a2c1021167c66556f9b1b6719"},
            {60245u, "000096d34679e3b6d5dcc1d7f7430f252318a6c7f460ee694702bb5ee14a96b1"},
            {12480u, "0000efaac2a3cb517f9c69bcaf0ff2d3bbcc9aff6a55488435c8e04998f71b58"},
            {191249u, "0000f341823da1c4b5fee5d2d72d098a7cb447cb24d59fec5ca72e0d7a4cdfac"},
            {167u, "000009ff814c3735478d4db792b7171d0ea9c90af35553daa654324444158790"},
            {145326u, "00002150f42ec729234446693a4e1c63d481c535dde8965d9e0c775f60a48133"},
            {56876u, "00004a39063493d6cc6f797924fc9fb2e8fb050af8a051014ad7361c745dd0c2"},
        };
        if(height>0 && height<=sizeof(vectors)/sizeof(vectors[0])) {
            auto candidate=header;
            const auto& vector=vectors[height-1];
            candidate.nonce=vector.nonce;
            if(candidate.GetHash().GetHex()==vector.hash) {
                Require(consensus::CheckProofOfWork(candidate,false));
                return candidate;
            }
        }
        return SolveHeader(header);
    }
    std::vector<Block> BuildOwnedChain(bool real_pow=false) {
        secret.back()=9;
        std::unique_ptr<secp256k1_context,decltype(&secp256k1_context_destroy)> ctx(
            secp256k1_context_create(SECP256K1_CONTEXT_NONE),secp256k1_context_destroy);
        secp256k1_keypair pair;secp256k1_xonly_pubkey key;
        Require(secp256k1_keypair_create(ctx.get(),&pair,secret.data()));
        Require(secp256k1_keypair_xonly_pub(ctx.get(),&key,nullptr,&pair));
        script={0x51,32};script.resize(34);
        Require(secp256k1_xonly_pubkey_serialize(ctx.get(),script.data()+2,&key));
        std::vector<Block> chain;consensus::ConsensusUTXOSet coins;
        consensus::BlockValidator validator(&coins);SeedDirect(coins);
        auto parent=SelectedGenesis().GetHash();
        std::deque<consensus::HeaderIndexEntry> headers;
        if(real_pow)headers.emplace_back(SelectedGenesis().header,nullptr);
        for(uint32_t height=1;height<=101;++height) {
            Block block;block.header.version=1;block.header.prev_block_hash=parent;
            block.header.timestamp=SelectedGenesis().header.timestamp+height*120;
            block.header.difficulty=0x1d00ffff;block.header.ZeroReserved();
            if(real_pow) {
                block.header.difficulty=GetNextWorkRequiredForCandidate(height,block.header.timestamp,
                    GetConsensusForCurrentNetwork(),static_cast<const CBlockIndex*>(nullptr),
                    &headers.back(),static_cast<NoChainDb*>(nullptr));
                Require(block.header.difficulty!=0);
            }
            block.vtx={MakeCoinbase(height)};block.vtx.front().vout.front().scriptPubKey=script;
            if(height==101 && historical_spend!=HistoricalSpend::None) {
                OutPoint point(chain.at(0).vtx.front().GetTxid(),0);
                const auto* original=coins.GetCoin(point);
                Require(original && original->isCoinbase && original->height==1);
                auto coin=*original;
                const auto append_spend=[&]() {
                    constexpr uint64_t fee=1000;
                    Require(coin.value.GetUna()>fee);
                    Transaction tx;tx.version=2;tx.witness_version=1;
                    TxInput input;input.prevout={point.txid,point.vout};input.sequence=0xfffffffe;
                    tx.vin.push_back(input);tx.vout.emplace_back(AmountUna::Una(coin.value.GetUna()-fee),script);
                    CanonicalWalletUTXO resolved;resolved.txid=point.txid.AsUint256();resolved.vout=point.vout;
                    resolved.value=coin.value;resolved.spk=coin.scriptPubKey;resolved.height=coin.height;
                    resolved.is_coinbase=coin.isCoinbase;
                    const auto digest=TaprootTxSigner::ComputeTaprootSighash(tx,0,{resolved},TaprootTxSigner::SIGHASH_DEFAULT);
                    Require(digest.size()==32);std::array<uint8_t,32> message{};
                    std::copy(digest.begin(),digest.end(),message.begin());std::array<uint8_t,64> signature{};
                    Require(TaprootKeys::SignSchnorr(signature,message,secret));
                    tx.vin.front().witness={std::vector<uint8_t>(signature.begin(),signature.end())};
                    point=OutPoint(tx.GetTxid(),0);
                    coin=consensus::UTXOEntry(tx.vout.front().value,script,height,false);
                    block.vtx.push_back(std::move(tx));
                };
                append_spend();
                if(historical_spend==HistoricalSpend::SameBlockChild)append_spend();
                block.vtx.front().vout.emplace_back(AmountUna::Zero(),
                    consensus::BuildWitnessCommitmentFromRoot(consensus::ComputeWitnessMerkleRoot(block.vtx)));
            }
            // Commit the actual historical output filter before txid, Merkle,
            // forest and PoW construction. OP_RETURN is excluded from the filter.
            const auto filter=consensus::GCSFilter::Build({script},parent);
            TxOutput commitment;commitment.value=AmountUna::Zero();
            commitment.scriptPubKey=consensus::BuildFilterCommitmentScript(filter.GetHash());
            block.vtx.front().vout.push_back(std::move(commitment));
            std::string filter_error;
            Require(consensus::ValidateFilterCommitment(block.vtx.front(),filter.GetHash(),height,filter_error));
            block.header.merkle_root=consensus::ComputeMerkleRoot(block.vtx);
            uint256 root;std::string error;
            Require(validator.ComputeUtreexoRootPure(block,height,root,error));block.header.utreexo_root=root;
            if(real_pow) {
                block.header=SolveHistoricalHeader(block.header,height);
                headers.emplace_back(block.header,&headers.back());
            }
            consensus::BlockUndo undo;Require(validator.ConnectBlock(block,height,block.GetHash(),undo,error));
            parent=block.GetHash();chain.push_back(std::move(block));
        }
        return chain;
    }
    MempoolTransaction Shield() {
        return Shield(historical_spend==HistoricalSpend::None?1:2);
    }
    MempoolTransaction Shield(uint32_t coin_height) {
        const auto point=OutPoint(blocks.at(coin_height).vtx.front().GetTxid(),0);
        const auto& coin=replay->ProvenUtxos().at(point);
        orchard::SigningDomain domain;domain.network_code=2;domain.branch_id=Params().orchard_branch_id;
        const auto genesis=uint256::FromHexUnsafe(Params().genesis_hash);
        std::copy(genesis.begin(),genesis.end(),domain.genesis_wire.begin());
        orchard::EnvelopeInput input{};std::copy(point.txid.AsUint256().begin(),point.txid.AsUint256().end(),input.txid_wire.begin());
        input.output_index=0;input.sequence=UINT32_MAX;
        constexpr uint64_t fee=100000,shield=5000;
        std::vector<orchard::TransparentOutput> outputs{{coin.value.GetUna()-fee-shield,script}};
        std::array<uint8_t,64> seed{51};auto keys=orchard::WalletKeys::FromSeed(seed,0);
        const std::vector<orchard::WalletPayment> payments{{shield,keys.Receiver(orchard::WalletScope::External,{})}};
        auto plan=orchard::WalletBundlePlan::PrepareShield(keys,payments);
        const std::vector<orchard::ResolvedInput> resolved{{input.txid_wire,0,input.sequence,coin.value.GetUna(),script}};
        const auto signing=orchard::SigningContext::Create(domain,0,resolved,outputs,fee);
        const auto bundle=std::move(plan).Prove(signing);last_bundle=bundle.Bytes();
        auto envelope=orchard::TransactionEnvelope::Create(0,{input},outputs,fee,bundle.Bytes());
        FirstBoundaryView view(*replay);
        const auto snapshot=consensus::OrchardCoinSnapshot::ResolveUnderChainstateLock(envelope,view);
        const auto digest=consensus::OrchardTransparentSigningDigest(snapshot,domain,0);
        std::unique_ptr<secp256k1_context,decltype(&secp256k1_context_destroy)> ctx(
            secp256k1_context_create(SECP256K1_CONTEXT_NONE),secp256k1_context_destroy);
        secp256k1_keypair pair;Require(secp256k1_keypair_create(ctx.get(),&pair,secret.data()));
        std::vector<uint8_t> signature(64);orchard::Hash aux{};
        Require(secp256k1_schnorrsig_sign32(ctx.get(),signature.data(),digest.data(),&pair,aux.data()));
        input.witness={signature};
        return MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::Create(0,{input},outputs,fee,bundle.Bytes()));
    }
    explicit OrchardAdmissionFixture(bool real_pow=false,HistoricalSpend spend=HistoricalSpend::None)
        :historical_spend(spend) {
        if(real_pow) {
            Require(Params().name=="regtest");
            MutableParams().regtest_enforce_pow=true;
            // This isolated catalog/reindex fixture crosses the actual timing
            // rules with Orchard. Historical blocks retain their old cadence.
            MutableParams().sixty_second_activation_height=102;
        }
        MutableParams().orchard_activation_height=102;
        MutableParams().orchard_branch_id=1;
        MutableParams().shielded_activation_height=1;
        MutableParams().shielded_epoch_reset_height=UINT32_MAX;
        MutableParams().shielded_spend_auth_epoch_reset_height=UINT32_MAX;
        GetConfig().utreexo_stateless=false;
        auto name=(std::filesystem::temp_directory_path()/"orchard_admission_XXXXXX").string();
        Require(mkdtemp(name.data())!=nullptr);
        // Reindex intentionally refuses symlinked database paths. Resolve the
        // fixture-owned temp directory (macOS /tmp is a symlink) before opening.
        path=real_pow?std::filesystem::canonical(name):std::filesystem::path(name);
        // Generated separated-layout fixture. Normal ChainDB open correctly
        // creates the legacy layout and must never silently migrate it.
        {
            auto families=shielded_store_fixture::legacy;
            families.push_back(shielded_store_fixture::shielded);
            shielded_store_fixture::Raw raw(path,families);
            const uint32_t schema=4;
            raw.put("meta","schema_version",std::string(reinterpret_cast<const char*>(&schema),4));
            raw.put("meta","storage_layout_v1",shielded_store_fixture::ready);
            consensus::shielded::CommitmentTree tree;consensus::shielded::AnchorHistory anchors;
            const auto frontier=tree.SerializeFrontier();const auto history=anchors.SerializePersistenceBytes();
            raw.put(shielded_store_fixture::shielded,"Mshielded_frontier",{frontier.begin(),frontier.end()});
            raw.put(shielded_store_fixture::shielded,"Mshielded_anchor_history",{history.begin(),history.end()});
            raw.put("meta","shielded_tip",std::string(84,'\0'));
        }
        Require(db.init(path)==Status::Ok);
        blocks={SelectedGenesis()};const auto rest=BuildOwnedChain(real_pow);
        Require(rest.size()==101);blocks.insert(blocks.end(),rest.begin(),rest.end());
        replay=std::make_unique<assumeutxo::AssumeUtxoReplayEngine>();
        std::string error;Require(replay->SeedGenesis(blocks.front(),error));
        arith_uint256 work{0};
        for(uint32_t h=0;h<blocks.size();++h) {
            const auto& b=blocks[h];
            if(h) Require(replay->ConnectAndAdvance(b,h,b.GetHash(),error));
            work+=GetBlockProof(b.header.difficulty);
            Require(db.putHeader(token,b.GetHash(),b.header,h,work)==Status::Ok);
            Require(db.putHeightIndex(token,h,b.GetHash())==Status::Ok);
            Require(db.putBlock(token,b.GetHash(),b)==Status::Ok);
            // Match genesis_init and BlockReindexer::seedGenesis: no genesis tx index.
            if(h)for(uint32_t i=0;i<b.vtx.size();++i)
                Require(db.putTxIndex(token,b.vtx[i].GetTxid().AsUint256(),b.GetHash(),i)==Status::Ok);
        }
        tip=CBlockIndex(blocks.back().header,101);tip.chainwork=work.GetHex();
        Require(db.setTip(token,tip.hash,tip.height,work)==Status::Ok);
        Require(db.setValidatedTip(token,tip.hash,tip.height)==Status::Ok);
        for(const auto& [point,entry]:replay->ProvenUtxos()) {
            Coin c;c.amount=entry.value.GetUna();c.script_pubkey=util::hex(entry.scriptPubKey);
            c.height=entry.height;c.coinbase=entry.isCoinbase;c.is_confidential=entry.is_confidential;c.commitment=entry.commitment;
            Require(db.putCoin(token,point.txid.AsUint256(),point.vout,c)==Status::Ok);
        }
        Require(db.putForestTipMarker(token,{101,tip.hash,uint256::FromHexUnsafe(replay->UtreexoRootHex())})==Status::Ok);
        const auto frontier=replay->ShieldedTree()->SerializeFrontier();
        const auto anchors=replay->ShieldedAnchors()->SerializePersistenceBytes();
        Require(db.putShieldedState(token,ChainDB::ShieldedStateRecord::Frontier,{frontier.begin(),frontier.end()})==Status::Ok);
        Require(db.putShieldedState(token,ChainDB::ShieldedStateRecord::AnchorHistory,{anchors.begin(),anchors.end()})==Status::Ok);
        const auto root=replay->ShieldedTree()->Root();uint256 tree_root;std::copy(root.begin(),root.end(),tree_root.begin());
        Require(db.putShieldedTipMarker(token,{101,tip.hash,tree_root,replay->ShieldedTree()->Size(),0})==Status::Ok);
        service->setOwnedChainDB(database);ShieldedStateStartupTestAccess::BoundaryState(*service,tip,*replay);
        auto headers=std::make_shared<consensus::HeaderChainSelector>();
        for(const auto& block:blocks)Require(headers->AddHeader(block.header));
        service->setHeaderChainSelector(headers);
        service->setRuntimeBlockNotifications(std::make_shared<AdmissionNotifications>());
        DaemonContext context;context.chainstate=service;context.config=std::make_shared<ConfigService>();
        context.logger_interface=logger.get();ingress=std::make_shared<MempoolService>();Require(ingress->Init(context));
    }
    ~OrchardAdmissionFixture() {
        ingress->Stop();ingress.reset();service->setRuntimeBlockNotifications(nullptr);service->setChainDB(nullptr);db.close();std::error_code ec;std::filesystem::remove_all(path,ec);
        MutableParams()=previous_params;GetConfig().utreexo_stateless=previous_stateless;
    }
    auto Read() {return ShieldedStateStartupTestAccess::Boundary(*service);}
    void CheckUnpublished() {
        ASSERT_EQ(db.getLegacyRetirementState().status(),Status::NotFound);
        ASSERT_TRUE(db.getTip().ok());EXPECT_EQ(db.getTip()->hash,tip.hash);
    }
};
}
TEST(OrchardSelectedAdmission, RealShieldPreflightPublicationAndDuplicate) {
    OrchardAdmissionFixture f;const auto body=f.Shield();auto& pool=f.ingress->mempool();
    unsigned observed=0;
    pool.setTxBodyAcceptedCallback([&](const MempoolTransaction& captured) {
        ++observed;EXPECT_EQ(captured.Serialize(),body.Serialize());EXPECT_TRUE(pool.hasTransaction(body.GetTxid().AsUint256()));
    });
    ASSERT_TRUE(pool.submitBody(body,"preflight",false,true).accepted());EXPECT_EQ(pool.size(),0u);EXPECT_EQ(observed,0u);
    const auto result=f.ingress->SubmitBody(body,TxOrigin::INTERNAL);ASSERT_TRUE(result.accepted())<<result.message;
    EXPECT_EQ(observed,1u);const auto entry=pool.getMempoolEntry(body.GetTxid().AsUint256());ASSERT_TRUE(entry);
    EXPECT_EQ(entry->height,101u);EXPECT_EQ(entry->fee,100000u);EXPECT_EQ(entry->tx.Serialize(),body.Serialize());
    EXPECT_TRUE(pool.isOutputSpentInMempool(body.Inputs().front()));
    EXPECT_EQ(f.ingress->SubmitBody(body,TxOrigin::INTERNAL).code,TxRejectCode::ALREADY_IN_MEMPOOL);
    EXPECT_EQ(observed,1u);f.CheckUnpublished();
}
TEST(OrchardSelectedAdmission, MissingOwnerAndInvalidInputRefuseThenRetry) {
    OrchardAdmissionFixture f;const auto body=f.Shield();auto& pool=f.ingress->mempool();
    f.service->setRuntimeBlockNotifications(nullptr);
    EXPECT_EQ(pool.submitBody(body,"provider",false).code,TxRejectCode::UNAVAILABLE);EXPECT_EQ(pool.size(),0u);
    f.service->setRuntimeBlockNotifications(std::make_shared<OrchardAdmissionFixture::AdmissionNotifications>());
    pool.setTxAcceptedCallback([](const Transaction&){});
    EXPECT_EQ(pool.submitBody(body,"observer",false).code,TxRejectCode::UNAVAILABLE);EXPECT_EQ(pool.size(),0u);
    pool.setTxAcceptedCallback({});
    const auto id=f.tip.hash;ASSERT_EQ(f.db.setValidatedTip(f.token,f.blocks[100].GetHash(),100),Status::Ok);
    EXPECT_EQ(pool.submitBody(body,"identity",false).code,TxRejectCode::UNAVAILABLE);EXPECT_EQ(pool.size(),0u);
    ASSERT_EQ(f.db.setValidatedTip(f.token,id,101),Status::Ok);
    const auto& tx=body.Orchard();auto inputs=tx.Inputs();inputs[0].witness[0][0]^=1;
    const auto bad=MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::Create(tx.LockTime(),inputs,tx.Outputs(),tx.ExplicitFee(),f.last_bundle));
    EXPECT_FALSE(pool.submitBody(bad,"signature",false).accepted());EXPECT_EQ(pool.size(),0u);
    ASSERT_TRUE(f.ingress->SubmitBody(body,TxOrigin::INTERNAL).accepted());f.CheckUnpublished();
}
TEST(OrchardSelectedAdmission, PublicationRollbackAndPostCommitObserverOutcome) {
    OrchardAdmissionFixture f;const auto body=f.Shield();auto& pool=f.ingress->mempool();
    f.logger->refuse=true;EXPECT_THROW(pool.submitBody(body,"rollback",false),std::runtime_error);
    EXPECT_EQ(pool.size(),0u);EXPECT_FALSE(pool.isOutputSpentInMempool(body.Inputs().front()));
    f.logger->refuse=false;
    pool.setTxBodyAcceptedCallback([&](const MempoolTransaction&){EXPECT_EQ(pool.size(),1u);throw std::runtime_error("fixture after-publication");});
    EXPECT_THROW(f.ingress->SubmitBody(body,TxOrigin::INTERNAL),std::runtime_error);
    EXPECT_TRUE(pool.hasTransaction(body.GetTxid().AsUint256()));EXPECT_TRUE(pool.isOutputSpentInMempool(body.Inputs().front()));f.CheckUnpublished();
}
TEST(OrchardSelectedAdmission, MatureInputsPendingConflictsAndSelectedDomain) {
    OrchardAdmissionFixture f;auto& pool=f.ingress->mempool();
    const auto immature=f.Shield(3);
    EXPECT_EQ(pool.submitBody(immature,"maturity",false).code,TxRejectCode::SCRIPT_VERIFY_FAILED);
    EXPECT_EQ(pool.size(),0u);
    const auto first=f.Shield(1);
    const auto branch=Params().orchard_branch_id;MutableParams().orchard_branch_id=branch+1;
    EXPECT_FALSE(pool.submitBody(first,"domain",false).accepted());EXPECT_EQ(pool.size(),0u);
    MutableParams().orchard_branch_id=branch;
    ASSERT_TRUE(f.ingress->SubmitBody(first,TxOrigin::INTERNAL).accepted());
    const auto second=f.Shield(2);
    const auto& tx=second.Orchard();auto copied=tx.Inputs();copied[0].txid_wire=first.Orchard().Inputs()[0].txid_wire;
    const auto conflict=MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::Create(0,copied,tx.Outputs(),tx.ExplicitFee(),f.last_bundle));
    EXPECT_EQ(pool.submitBody(conflict,"input-owner",false).code,TxRejectCode::DOUBLE_SPEND_NO_RBF);
    const auto result=f.ingress->SubmitBody(second,TxOrigin::INTERNAL);ASSERT_TRUE(result.accepted())<<result.message;
    EXPECT_EQ(pool.size(),2u);EXPECT_TRUE(pool.hasTransaction(first.GetTxid().AsUint256()));
    auto changed=tx.Inputs();changed[0].sequence=0xfffffffe;
    // Remove transparent-input overlap while retaining every actual action
    // nullifier. No signature/proof validity is claimed for this refused body.
    auto other_inputs=changed;other_inputs[0].txid_wire=immature.Orchard().Inputs()[0].txid_wire;
    const auto same_nullifiers=MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::Create(0,other_inputs,tx.Outputs(),tx.ExplicitFee(),f.last_bundle));
    EXPECT_EQ(pool.submitBody(same_nullifiers,"nullifier-owner",false).code,TxRejectCode::DOUBLE_SPEND_NO_RBF);
    EXPECT_EQ(pool.size(),2u);f.CheckUnpublished();
}
#else
TEST(OrchardSelectedAdmission, DefaultValidatorUnavailable) {
    MempoolChainstateReadGuard owner;
    EXPECT_EQ(owner.ValidateOrchard(MempoolTransaction{},{}).result.code,TxRejectCode::UNAVAILABLE);
}
#endif
