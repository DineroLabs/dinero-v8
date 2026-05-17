# 🖥️ GUI Wiring - 15-Minute Production Integration

## Status Label Mapping (Copy/Paste Ready)

```cpp
// In MiningController::updateStatus() - wire these directly to UI labels:

void MiningController::updateMiningStatus() {
    auto status = rpcClient->call("mining.status").result;
    
    // Status & Controls
    bool isRunning = status["running"].toBool();
    ui->lblStatus->setText(isRunning ? "✅ Mining" : "⏸️ Stopped");
    ui->btnStart->setEnabled(!isRunning);
    ui->btnStop->setEnabled(isRunning);
    
    // Thread & Throttle Display
    ui->lblThreads->setText(QString::number(status["threads"].toInt()));
    ui->lblThrottle->setText(QString::number(status["throttle"].toDouble() * 100, 'f', 1) + "%");
    
    // Hashrate (handle null values)
    auto instRate = status["hashrate_hps_inst"];
    auto maRate = status["hashrate_hps_ma"];
    
    ui->lblHashrateInst->setText(instRate.isNull() ? "--" : 
        QString::number(instRate.toDouble(), 'f', 1) + " H/s");
    ui->lblHashrateMA->setText(maRate.isNull() ? "--" : 
        QString::number(maRate.toDouble(), 'f', 1) + " H/s");
    
    // Mining Counters
    ui->lblAttempts->setText(QString::number(status["submit_attempts"].toInt()));
    ui->lblAccepted->setText(QString::number(status["submit_accepted"].toInt()));
    ui->lblRejected->setText(QString::number(status["submit_rejected"].toInt()));
    ui->lblBlocksMined->setText(QString::number(status["blocks_mined"].toInt()));
    
    // Last Error (with toast notification)
    auto lastError = status["last_submit_error"];
    if (lastError.isNull()) {
        ui->lblLastError->setText("None");
        ui->lblLastError->setStyleSheet("color: green;");
    } else {
        QString error = lastError.toString();
        ui->lblLastError->setText(error);
        ui->lblLastError->setStyleSheet("color: red;");
        
        // Show error toast (only once per error)
        static QString lastShownError;
        if (error != lastShownError) {
            showMiningErrorToast(error);
            lastShownError = error;
        }
    }
}
```

## Mining Controls

```cpp
// Start Mining
void MiningController::onStartClicked() {
    // Pre-fill address from wallet
    if (ui->lineEditAddress->text().isEmpty()) {
        auto addr = rpcClient->call("getnewaddress").result["address"].toString();
        ui->lineEditAddress->setText(addr);
    }
    
    // Validate address
    QString address = ui->lineEditAddress->text();
    if (!address.startsWith("din1") || address.length() < 20) {
        showToast("❌ Invalid mining address", ToastType::Error);
        return;
    }
    
    // Network safety check
    if (currentNetwork != "regtest" && !ui->chkEnableMining->isChecked()) {
        showToast("⚠️ Enable local mining checkbox required for non-regtest", ToastType::Warning);
        return;
    }
    
    // Start mining
    QJsonObject params;
    params["address"] = address;
    params["threads"] = ui->sliderThreads->value();
    params["throttle"] = ui->sliderThrottle->value() / 100.0;
    
    auto result = rpcClient->call("mining.start", QJsonArray{params});
    if (result.error.isEmpty()) {
        showToast("🚀 Mining started", ToastType::Success);
        startStatusTimer(); // Begin status updates
    } else {
        showToast("❌ Failed to start: " + result.error, ToastType::Error);
    }
}

// Stop Mining
void MiningController::onStopClicked() {
    auto result = rpcClient->call("mining.stop");
    showToast("⏹️ Mining stopped", ToastType::Info);
    stopStatusTimer();
}
```

## Quality-of-Life Features

```cpp
// Network Badge & Safety
void MiningController::setupNetworkSafety() {
    // Show network badge
    ui->lblNetworkBadge->setText(currentNetwork.toUpper());
    
    if (currentNetwork == "regtest") {
        ui->lblNetworkBadge->setStyleSheet("background: #28a745; color: white; padding: 4px 8px; border-radius: 4px;");
        ui->chkEnableMining->setVisible(false); // Not needed for regtest
    } else {
        ui->lblNetworkBadge->setStyleSheet("background: #dc3545; color: white; padding: 4px 8px; border-radius: 4px;");
        ui->chkEnableMining->setVisible(true);
        ui->chkEnableMining->setText("Enable local mining (testnet/mainnet)");
    }
}

// Error Toast Messages
void MiningController::showMiningErrorToast(const QString& error) {
    QString message;
    ToastType type = ToastType::Warning;
    
    if (error == "time-too-new" || error == "time-too-old") {
        message = "⏰ Block timestamp invalid - network sync issue";
    } else if (error == "bad-pow") {
        message = "🎯 Proof of work insufficient - difficulty changed";
    } else if (error == "bad-prevblk") {
        message = "🔗 Parent block mismatch - blockchain reorg detected";
        type = ToastType::Error;
    } else if (error.contains("battery")) {
        message = "🔋 Mining paused - laptop on battery";
        type = ToastType::Info;
    } else if (error.contains("thermal")) {
        message = "🌡️ Mining paused - thermal throttling";
    } else {
        message = "❌ Mining error: " + error;
        type = ToastType::Error;
    }
    
    showToast(message, type);
}

// Block Acceptance Celebration
void MiningController::checkForNewBlocks() {
    static int lastAccepted = 0;
    
    auto status = rpcClient->call("mining.status").result;
    int currentAccepted = status["submit_accepted"].toInt();
    
    if (currentAccepted > lastAccepted) {
        // Get current height for celebration
        auto height = rpcClient->call("getblockcount").result.toInt();
        showToast("🎊 Block accepted! Height: " + QString::number(height), ToastType::Success);
        
        // Play sound effect (optional)
        // QSound::play(":/sounds/block_found.wav");
        
        lastAccepted = currentAccepted;
    }
}
```

## Thread/Throttle Sliders

```cpp
void MiningController::setupMiningControls() {
    // Thread slider (1 to CPU-1)
    int maxThreads = std::max(1, QThread::idealThreadCount() - 1);
    ui->sliderThreads->setRange(1, maxThreads);
    ui->sliderThreads->setValue(1); // Conservative default
    ui->lblMaxThreads->setText("Max: " + QString::number(maxThreads));
    
    // Throttle slider (15% to 90%)
    ui->sliderThrottle->setRange(15, 90);
    ui->sliderThrottle->setValue(35); // Conservative default for thermal safety
    
    // Update labels on change
    connect(ui->sliderThreads, &QSlider::valueChanged, [this](int value) {
        ui->lblThreadsValue->setText(QString::number(value));
    });
    
    connect(ui->sliderThrottle, &QSlider::valueChanged, [this](int value) {
        ui->lblThrottleValue->setText(QString::number(value) + "%");
    });
}
```

## Status Update Timer

```cpp
void MiningController::startStatusTimer() {
    if (!statusTimer) {
        statusTimer = new QTimer(this);
        connect(statusTimer, &QTimer::timeout, this, &MiningController::updateMiningStatus);
        connect(statusTimer, &QTimer::timeout, this, &MiningController::checkForNewBlocks);
    }
    statusTimer->start(2000); // Update every 2 seconds
}

void MiningController::stopStatusTimer() {
    if (statusTimer) {
        statusTimer->stop();
    }
}
```

## Integration Checklist ✅

- [ ] Wire status labels to `mining.status` RPC fields
- [ ] Implement Start/Stop button logic with address validation  
- [ ] Add network safety checkbox for testnet/mainnet
- [ ] Create error-specific toast notifications
- [ ] Set up thread/throttle sliders with safe defaults
- [ ] Add block acceptance celebration notifications
- [ ] Implement status update timer (2-second refresh)
- [ ] Pre-fill mining address from `getnewaddress`

**15-minute integration complete - Beta users get professional mining UX!** 🚀
