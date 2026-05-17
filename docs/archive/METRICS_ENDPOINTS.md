# 📊 Metrics & Polish - Production Monitoring

## /metrics Endpoint (Prometheus Format)
```cpp
// Add to main.cpp RPC handlers
g_rpcRegistry.registerHandler("metrics", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
    std::ostringstream metrics;
    
    // Mining metrics
    metrics << "# HELP din_blocks_accepted_total Number of blocks accepted\n";
    metrics << "# TYPE din_blocks_accepted_total counter\n";
    metrics << "din_blocks_accepted_total " << g_submitAccepted.load() << "\n";
    
    metrics << "# HELP din_blocks_rejected_total Number of blocks rejected\n";
    metrics << "# TYPE din_blocks_rejected_total counter\n";
    metrics << "din_blocks_rejected_total{reason=\"" 
            << (g_lastSubmitError.empty() ? "none" : g_lastSubmitError) 
            << "\"} " << g_submitRejected.load() << "\n";
    
    metrics << "# HELP din_hashrate_hps Current hashrate\n";
    metrics << "# TYPE din_hashrate_hps gauge\n";
    if (g_miningEngine) {
        auto stats = g_miningEngine->GetStats();
        metrics << "din_hashrate_hps " << stats.hashrateHps.load() << "\n";
    }
    
    metrics << "# HELP din_blockchain_height Current height\n";
    metrics << "# TYPE din_blockchain_height gauge\n";
    auto height = dinero::db::ReadMeta("height");
    metrics << "din_blockchain_height " << (height.empty() ? "0" : height) << "\n";
    
    return din::Json(metrics.str());
});
```

## Database Assertions (Integrity Checks)
```cpp
// Add to BlockAcceptor::ConnectBlock after successful insert
void BlockAcceptor::ValidateBlockIntegrity(const ParsedBlock& block, uint64_t height) {
    // Verify height progression
    auto parentHeight = height - 1;
    if (height > 1) {
        auto storedParent = GetBlockByHeight(parentHeight);
        if (storedParent.hash != block.prevBlockHash) {
            LOG_ERROR("❌ INTEGRITY VIOLATION: Parent hash mismatch at height " + std::to_string(height));
            throw std::runtime_error("Block integrity check failed");
        }
    }
    
    // Verify chainwork advancement
    if (height > 0) {
        auto parentChainwork = GetChainworkAtHeight(parentHeight);
        auto currentChainwork = GetChainworkAtHeight(height);
        if (currentChainwork <= parentChainwork) {
            LOG_ERROR("❌ INTEGRITY VIOLATION: Chainwork did not advance at height " + std::to_string(height));
            throw std::runtime_error("Chainwork integrity check failed");
        }
    }
    
    LOG_INFO("✅ Block integrity verified at height " + std::to_string(height));
}
```

## Wallet Integration (Coinbase Maturity)
```cpp
// Show pending coinbase rewards in wallet
class CoinbaseTracker {
public:
    struct PendingCoinbase {
        std::string txid;
        uint64_t amount;
        uint64_t blockHeight;
        uint64_t maturesAtHeight;
        bool isMature(uint64_t currentHeight) const {
            return currentHeight >= maturesAtHeight;
        }
    };
    
    std::vector<PendingCoinbase> getPendingCoinbases(const std::string& address) {
        // Query block_index for coinbase transactions to our address
        // Calculate maturity (height + 100 blocks)
        // Return pending/mature status
    }
};

// In GUI wallet tab
void WalletController::updateCoinbaseStatus() {
    auto pending = coinbaseTracker.getPendingCoinbases(miningAddress);
    
    ui->tableCoinbase->clear();
    for (const auto& cb : pending) {
        auto row = ui->tableCoinbase->rowCount();
        ui->tableCoinbase->insertRow(row);
        
        ui->tableCoinbase->setItem(row, 0, new QTableWidgetItem(
            QString::fromStdString(cb.txid.substr(0, 16) + "...")));
        ui->tableCoinbase->setItem(row, 1, new QTableWidgetItem(
            QString::number(cb.amount / 1e8, 'f', 8) + " DIN"));
        
        if (cb.isMature(getCurrentHeight())) {
            ui->tableCoinbase->setItem(row, 2, new QTableWidgetItem("✅ Mature"));
        } else {
            auto remaining = cb.maturesAtHeight - getCurrentHeight();
            ui->tableCoinbase->setItem(row, 2, new QTableWidgetItem(
                "⏳ " + QString::number(remaining) + " blocks"));
        }
    }
}
```

## Quick Monitoring Commands
```bash
# Check mining metrics
curl -s http://127.0.0.1:20999/metrics | grep din_

# Monitor block acceptance rate
watch -n 1 'curl -s http://127.0.0.1:20999/metrics | grep blocks_accepted'

# Check hashrate
curl -s http://127.0.0.1:20999/metrics | grep hashrate

# Verify blockchain height advancing
watch -n 5 'curl -s http://127.0.0.1:20999/metrics | grep blockchain_height'
```

## Health Check Endpoint
```cpp
// Add /healthz for load balancer health checks
g_rpcRegistry.registerHandler("healthz", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
    din::Json health = din::obj();
    
    health["status"] = "healthy";
    health["height"] = std::stoll(dinero::db::ReadMeta("height"));
    health["database"] = dinero::db::GetDatabase() ? "connected" : "disconnected";
    health["uptime"] = getUptime();
    
    if (g_miningEngine) {
        auto stats = g_miningEngine->GetStats();
        health["mining"] = stats.running.load() ? "active" : "inactive";
    }
    
    return health;
});
```
