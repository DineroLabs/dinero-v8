# 🖥️ GUI Mining Integration - Wire to Real Backend

## Quick Wins for Mining Tab

### Mining Status Display
```cpp
// In MiningController::updateStatus()
auto status = mining.status();

// Wire these counters to GUI labels:
ui->lblAttempts->setText(QString::number(status.submit_attempts));
ui->lblAccepted->setText(QString::number(status.submit_accepted));  
ui->lblRejected->setText(QString::number(status.submit_rejected));
ui->lblBlocksMined->setText(QString::number(status.blocks_mined));

// Hashrate display (handle null values)
if (status.hashrate_hps_inst.isNull()) {
    ui->lblHashrateInst->setText("--");
} else {
    ui->lblHashrateInst->setText(QString::number(status.hashrate_hps_inst.toDouble(), 'f', 1) + " H/s");
}

if (status.hashrate_hps_ma.isNull()) {
    ui->lblHashrateMA->setText("--");
} else {
    ui->lblHashrateMA->setText(QString::number(status.hashrate_hps_ma.toDouble(), 'f', 1) + " H/s");
}

// Error display
if (status.last_submit_error.isNull()) {
    ui->lblLastError->setText("None");
    ui->lblLastError->setStyleSheet("color: green;");
} else {
    ui->lblLastError->setText(status.last_submit_error.toString());
    ui->lblLastError->setStyleSheet("color: red;");
}
```

### Address Picker Integration
```cpp
// Default to wallet receive address
void MiningController::setupAddressPicker() {
    auto addr = rpcClient->call("getnewaddress").result.address;
    ui->lineEditAddress->setText(addr);
}

// Validate before mining.start
bool MiningController::validateMiningAddress() {
    QString addr = ui->lineEditAddress->text();
    if (addr.isEmpty() || !addr.startsWith("din1")) {
        showToast("Invalid mining address", ToastType::Error);
        return false;
    }
    return true;
}
```

### Controls Integration
```cpp
// Start/Stop buttons
void MiningController::onStartClicked() {
    if (!validateMiningAddress()) return;
    
    QJsonObject params;
    params["address"] = ui->lineEditAddress->text();
    params["threads"] = ui->sliderThreads->value();
    params["throttle"] = ui->sliderThrottle->value() / 100.0;
    
    auto result = rpcClient->call("mining.start", QJsonArray{params});
    if (result.error.isEmpty()) {
        showToast("Mining started", ToastType::Success);
        ui->btnStart->setEnabled(false);
        ui->btnStop->setEnabled(true);
    } else {
        showToast("Failed to start: " + result.error, ToastType::Error);
    }
}

void MiningController::onStopClicked() {
    auto result = rpcClient->call("mining.stop");
    showToast("Mining stopped", ToastType::Info);
    ui->btnStart->setEnabled(true);
    ui->btnStop->setEnabled(false);
}
```

### Toast Notifications
```cpp
// Error-specific toasts based on last_submit_error
void MiningController::handleMiningError(const QString& error) {
    if (error == "time-too-new" || error == "time-too-old") {
        showToast("⏰ Block timestamp invalid", ToastType::Warning);
    } else if (error == "bad-pow") {
        showToast("🎯 Proof of work insufficient", ToastType::Warning);
    } else if (error == "bad-prevblk") {
        showToast("🔗 Parent block mismatch", ToastType::Error);
    } else if (error.contains("battery")) {
        showToast("🔋 Mining paused (battery mode)", ToastType::Info);
    } else if (error.contains("thermal")) {
        showToast("🌡️ Mining paused (thermal throttling)", ToastType::Warning);
    } else {
        showToast("❌ Mining error: " + error, ToastType::Error);
    }
}

// Success notifications
void MiningController::checkForNewBlocks() {
    static int lastAccepted = 0;
    int currentAccepted = getCurrentAcceptedCount();
    
    if (currentAccepted > lastAccepted) {
        showToast("🎊 Block accepted! Height: " + QString::number(getBlockHeight()), 
                 ToastType::Success);
        lastAccepted = currentAccepted;
    }
}
```

## Thread/Throttle Controls
```cpp
// Initialize sliders with safe defaults
void MiningController::setupControls() {
    int cpuCount = QThread::idealThreadCount();
    ui->sliderThreads->setRange(1, std::max(1, cpuCount - 1));
    ui->sliderThreads->setValue(1); // Conservative default
    
    ui->sliderThrottle->setRange(15, 90); // 15-90% throttle
    ui->sliderThrottle->setValue(35); // Conservative default
    
    connect(ui->sliderThreads, &QSlider::valueChanged, 
            this, &MiningController::onThreadsChanged);
    connect(ui->sliderThrottle, &QSlider::valueChanged,
            this, &MiningController::onThrottleChanged);
}

void MiningController::onThreadsChanged(int threads) {
    ui->lblThreadsValue->setText(QString::number(threads));
    // Update mining if running
    if (isMining()) {
        updateMiningParams();
    }
}
```
