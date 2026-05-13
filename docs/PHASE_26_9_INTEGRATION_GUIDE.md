# Phase 26.9: External Miner Integration Guide

## Overview

This guide shows how to wire existing mining infrastructure (Stratum servers, CPU miners, GPU miners) to the new **MiningCoordinator** architecture.

---

## Architecture

```
External Miners (Stratum/CPU/GPU)
    ↓
Worker Interfaces (CpuWorker, GpuWorker, StratumBridge)
    ↓
MiningCoordinator (job distribution, share validation)
    ↓
BlockTemplateManager (template creation)
    ↓
BlockAcceptor (block submission)
    ↓
Chain Database
```

---

## 1. Stratum V1 Server Integration

### Existing Code Location
`/src/stratum_bridge/stratum_server.cpp`

### Integration Steps

#### Step 1: Add MiningCoordinator to StratumServer

```cpp
// In include/stratum_bridge/stratum_server.h

#include "mining/mining_coordinator.h"
#include "mining/worker_interface.h"

class StratumServer {
public:
    // Add constructor parameter
    explicit StratumServer(
        DaemonContext* ctx,
        dinero::mining::MiningCoordinator* coordinator  // NEW
    );

private:
    dinero::mining::MiningCoordinator* coordinator_;  // NEW
    std::unique_ptr<dinero::mining::StratumWorkerBridge> worker_bridge_;  // NEW
};
```

#### Step 2: Initialize Worker Bridge

```cpp
// In src/stratum_bridge/stratum_server.cpp

StratumServer::StratumServer(
    DaemonContext* ctx,
    dinero::mining::MiningCoordinator* coordinator
)
    : daemon_ctx_(ctx)
    , coordinator_(coordinator)  // NEW
{
    // Create worker bridge
    worker_bridge_ = std::make_unique<dinero::mining::StratumWorkerBridge>(
        coordinator_
    );  // NEW
}
```

#### Step 3: Wire mining.subscribe

```cpp
// When client subscribes (mining.subscribe)
void StratumServer::handleSubscribe(const std::string& session_id) {
    // Notify worker bridge
    worker_bridge_->onWorkerConnected(
        session_id,
        "unknown",  // Worker name set during authorize
        false       // Stratum V1
    );

    // Get job for this session
    auto job = worker_bridge_->getJob(session_id);

    // Send mining.notify to client
    sendMiningNotify(session_id, job);
}
```

#### Step 4: Wire mining.authorize

```cpp
// When client authorizes (mining.authorize)
void StratumServer::handleAuthorize(
    const std::string& session_id,
    const std::string& worker_name
) {
    // Update worker name in bridge
    worker_bridge_->onWorkerConnected(
        session_id,
        worker_name,
        false  // Stratum V1
    );
}
```

#### Step 5: Wire mining.submit

```cpp
// When client submits share (mining.submit)
void StratumServer::handleSubmit(
    const std::string& session_id,
    const Json::Value& params
) {
    // Parse Stratum submit parameters
    std::string worker_name = params[0].asString();
    std::string job_id = params[1].asString();
    std::string extranonce2 = params[2].asString();
    std::string ntime = params[3].asString();
    std::string nonce = params[4].asString();

    // Create share submission
    dinero::mining::ShareSubmission share;
    share.job_id = job_id;
    share.worker_name = worker_name;
    share.extranonce2 = extranonce2;
    share.timestamp = std::stoul(ntime, nullptr, 16);
    share.nonce = std::stoul(nonce, nullptr, 16);

    // Submit to coordinator via bridge
    bool accepted = worker_bridge_->submitShare(session_id, share);

    // Send response to client
    if (accepted) {
        sendAcceptedShare(session_id);
    } else {
        sendRejectedShare(session_id, "low-difficulty");
    }
}
```

#### Step 6: Wire mining.set_difficulty (Vardiff)

```cpp
// Periodic difficulty adjustment
void StratumServer::updateDifficulty(const std::string& session_id) {
    // Get worker hashrate estimate
    double hashrate = estimateHashrate(session_id);

    // Get recommended difficulty from coordinator
    double recommended_diff = worker_bridge_->getRecommendedDifficulty(
        session_id,
        hashrate
    );

    // Set difficulty in coordinator
    worker_bridge_->setDifficulty(session_id, recommended_diff);

    // Send mining.set_difficulty to client
    sendSetDifficulty(session_id, recommended_diff);
}
```

#### Step 7: Wire disconnect

```cpp
void StratumServer::handleDisconnect(const std::string& session_id) {
    // Notify worker bridge
    worker_bridge_->onWorkerDisconnected(session_id);
}
```

---

## 2. CPU Miner Integration

### Existing Code Location
`/src/unified_miner/miner_manager.cpp`

### Integration Steps

#### Step 1: Add MiningCoordinator to MinerManager

```cpp
// In include/unified_miner/miner_manager.h

#include "mining/mining_coordinator.h"
#include "mining/worker_interface.h"

class MinerManager {
public:
    // Add method to set coordinator
    void setMiningCoordinator(dinero::mining::MiningCoordinator* coordinator);

private:
    dinero::mining::MiningCoordinator* coordinator_;
    std::vector<std::unique_ptr<dinero::mining::CpuWorker>> cpu_workers_;
};
```

#### Step 2: Start CPU Workers

```cpp
// In src/unified_miner/miner_manager.cpp

void MinerManager::startCpuMining(int thread_count) {
    if (!coordinator_) {
        throw std::runtime_error("MiningCoordinator not set");
    }

    // Create CPU worker
    std::string worker_id = "cpu_miner_" + std::to_string(cpu_workers_.size());

    auto worker = std::make_unique<dinero::mining::CpuWorker>(
        coordinator_,
        worker_id,
        thread_count
    );

    // Start worker
    if (!worker->start()) {
        throw std::runtime_error("Failed to start CPU worker");
    }

    cpu_workers_.push_back(std::move(worker));

    g_logger.info("CPU mining started with " + std::to_string(thread_count) + " threads");
}
```

#### Step 3: Stop CPU Workers

```cpp
void MinerManager::stopCpuMining() {
    for (auto& worker : cpu_workers_) {
        worker->stop();
    }
    cpu_workers_.clear();

    g_logger.info("CPU mining stopped");
}
```

---

## 3. GPU Miner Integration

### Existing Code Location
`/src/mining/gpu/gpu_device_manager.cpp`

### Integration Steps

#### Step 1: Add MiningCoordinator to GPU Manager

```cpp
// In GPU manager class

class GpuDeviceManager {
public:
    void setMiningCoordinator(dinero::mining::MiningCoordinator* coordinator);

private:
    dinero::mining::MiningCoordinator* coordinator_;
    std::vector<std::unique_ptr<dinero::mining::GpuWorker>> gpu_workers_;
};
```

#### Step 2: Start GPU Workers

```cpp
void GpuDeviceManager::startGpuMining(int device_id) {
    if (!coordinator_) {
        throw std::runtime_error("MiningCoordinator not set");
    }

    // Create GPU worker
    std::string worker_id = "gpu_device_" + std::to_string(device_id);

    auto worker = std::make_unique<dinero::mining::GpuWorker>(
        coordinator_,
        worker_id,
        device_id
    );

    // Start worker
    if (!worker->start()) {
        throw std::runtime_error("Failed to start GPU worker");
    }

    gpu_workers_.push_back(std::move(worker));

    g_logger.info("GPU mining started on device " + std::to_string(device_id));
}
```

---

## 4. Daemon Integration

### Wire Everything Together

```cpp
// In src/daemon/daemon_app.cpp

#include "mining/mining_coordinator.h"

class DaemonApp {
private:
    std::unique_ptr<dinero::mining::MiningCoordinator> mining_coordinator_;
    std::unique_ptr<StratumServer> stratum_server_;
    std::unique_ptr<MinerManager> miner_manager_;
};

bool DaemonApp::initialize() {
    // ... existing initialization ...

    // Create mining coordinator
    mining_coordinator_ = std::make_unique<dinero::mining::MiningCoordinator>(
        &daemon_context_
    );

    // Initialize Stratum server with coordinator
    stratum_server_ = std::make_unique<StratumServer>(
        &daemon_context_,
        mining_coordinator_.get()  // Pass coordinator
    );

    // Initialize miner manager with coordinator
    miner_manager_ = std::make_unique<MinerManager>();
    miner_manager_->setMiningCoordinator(mining_coordinator_.get());

    return true;
}
```

---

## 5. Testing

### Test Stratum V1 Connection

```bash
# Start daemon
./dinerod

# Connect with cgminer (or any Stratum miner)
cgminer -o stratum+tcp://127.0.0.1:3333 \
        -u worker1 \
        -p password
```

### Test CPU Mining

```bash
# Via RPC
./dinero-cli miner.start '{"address":"din1q...","threads":4}'

# Or via MinerManager
miner_manager_->startCpuMining(4);
```

### Test GPU Mining

```bash
# Via GPU manager
gpu_manager->startGpuMining(0);  // Device 0
```

---

## 6. Monitoring

### Get Coordinator Statistics

```cpp
auto stats = mining_coordinator_->getStats();

std::cout << "Total shares: " << stats.total_shares << std::endl;
std::cout << "Total blocks: " << stats.total_blocks << std::endl;
std::cout << "Total hashrate: " << stats.total_hashrate << " H/s" << std::endl;
std::cout << "Active workers: " << stats.active_workers << std::endl;
```

### Get Worker Statistics

```cpp
auto workers = mining_coordinator_->getWorkerStats();

for (const auto& worker : workers) {
    std::cout << "Worker: " << worker.worker_id << std::endl;
    std::cout << "  Type: " << static_cast<int>(worker.type) << std::endl;
    std::cout << "  Shares: " << worker.shares_accepted << std::endl;
    std::cout << "  Hashrate: " << worker.hashrate << " H/s" << std::endl;
    std::cout << "  Difficulty: " << worker.difficulty << std::endl;
}
```

---

## Summary

**Phase 26.9 provides:**

✅ **CpuWorker** - Integrates internal CPU mining with coordinator
✅ **GpuWorker** - Integrates GPU mining with coordinator
✅ **StratumWorkerBridge** - Bridges Stratum servers to coordinator
✅ **Unified interface** - All workers use same MiningCoordinator API
✅ **Share validation** - Automatic pool-style validation
✅ **Vardiff support** - Automatic difficulty adjustment per worker
✅ **Block submission** - Automatic when share meets network target

**This is exactly how Bitcoin mining pools work.** 🎉
