#pragma once

#include <QMainWindow>
#include <QMap>
#include <QJsonObject>
#include <QJsonValue>
#include <QVector>
#include <cstdint>
#include "debugconsole.h"
#include "connection_manager.h"
#include "minercontroller.h"

class RpcClient;
class ChangeAddressManager;
class TransactionTracker;
class AdvisoryBannerQueue;

class AiPanel;
class AiStatusStrip;
namespace dinero::qt::dashboard { class CmdKPanel; }
class DpiWidget;
class HardwareWalletWidget;
class QShortcut;

#ifdef DIN_EXPERIMENTAL_FEATURES
class WebSocketClient;
// class BridgeWidget;  // DISABLED - not ready for production
class PaymentsWidget;
class EscrowWidget;
class MarketplaceWidget;
#endif

// NOTE: Lightning Network moved to separate lightning-main branch (L2)
// namespace dinero { class LightningWidget; }  // REMOVED for L1 purity

class QLabel;
class QLineEdit;
class QPushButton;
class QTextEdit;
class QTableWidget;
class QTableWidgetItem;
class QProgressBar;
class QTabWidget;
class QTimer;
class QFileSystemWatcher;
class QWidget;
class QGroupBox;
class QGraphicsColorizeEffect;

#ifdef HAVE_QT_QUICK
class RpcHelper;
class QQuickWidget;
#endif

class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow(); // Destructor to clean up mining process
  void setDatadir(const QString& datadir);

  // Get debug console instance for message handler
  dinero::DebugConsole* getDebugConsole() const { return debugConsole_; }

private Q_SLOTS:
  void onRpcResult(const QString& method, const QJsonValue& result);
  void onRpcError(const QString& method, int code, const QString& message);
  void refresh();
  void onNewAddress();
  void onValidateAddress();
  void onCopyAddress();
  void onRefreshBlocks();
  void onStartMining();
  void onStopMining();
  void onToggleMining();  // v0.14.0.4: Toggle button handler
  void onSetMiningAddress();
  void onBrowseMinerBinary();     // Browse for external miner binary
  void onMiningModeChanged(int index); // Solo (default) vs Pool (Stratum)
  void onMinerTypeChanged(int index);  // Handle miner type selection change
  void onToggleLocalStratumServer(); // Start/stop localhost Stratum bridge
  void startInternalMiner(bool useGpu = false);      // Start internal RPC-based miner
  void startGPUMiner();           // Start GPU miner (Metal/CUDA/OpenCL)
  void startExternalMiner();      // Start external process-based miner (V1 stratum)
  void startSv2Miner();           // Start dinero-sv2-miner (SV2 + Job Declaration)
  void stopInternalMiner();       // Stop internal miner
  void stopExternalMiner();       // Stop external miner
  void stopLocalStratumServer();
  void onCreateWallet();
  void onWalletCreated(const QString& walletName, const QString& fingerprint, bool restored);
  void onExportSeed();

  // Daemon control slots
  void onStartDaemon();
  void onStopDaemon();
  void onBrowseDaemonBinary();      // Browse for dinerod binary
  void detectExistingDaemon();      // Intelligent daemon state detection

  // Wallet slots
  void onWalletLockToggle();  // Single toggle for lock/unlock
  void onLockWallet();
  void onUnlockWallet();
  void onEncryptWallet();
  void onLoadSelectedWallet();
  void onRescanWallet();
  void onDeriveNewAddress();
  void onUnlockCountdownTick();  // Update countdown timer display
  void checkRescanStatus();      // Check if wallet is rescanning
  void updateWalletUIState();    // Update all wallet-dependent UI elements
  bool isRescanSafeModeError(const QString& errorText) const;
  void scheduleSafeModeRescanRetry(const QString& errorText);
  void clearSafeModeRescanRetry();

  // Send tab slots
  void onSendTransaction();
  void onCreatePSBT();  // Create PSBT for hardware wallet
  void onListUTXOs();
  void onUseMaxAmount();
  void onFeePresetChanged(int index);  // Phase 35: Handle fee preset selection
  void updateFeeEstimate();            // Phase 35: Fetch and display fee estimate
  void onConsolidateUTXOs();           // Consolidate wallet UTXOs via RPC

  // Network verification
  void verifyProductionNetwork();

  // Peer table actions
  void onDisconnectPeer();
  void onBanPeer();
  void onReconnectAllPeers();
  void onCopyNetworkDiagnostics();

  // Template viewer
  void onRefreshTemplate();

  // QR Code generator
  void onGenerateQR();

  // Address book CSV
  void onImportAddresses();
  void onExportAddresses();

  // Monitoring Dashboard
  void onExportMetrics();
  void updateMonitoringDashboard();

  // WebSocket subscription handlers
  void onWsNewBlock(const QJsonObject& blockData);
  void onWsNewTransaction(const QJsonObject& txData);
  void onWsMiningInfo(const QJsonObject& miningData);
  void onWsNetworkInfo(const QJsonObject& networkData);
  void onWsMempoolUpdate(const QJsonObject& mempoolData);
  void onWsSyncProgress(const QJsonObject& syncData);
  void onWsConnected();
  void onWsDisconnected();
  void onWsError(const QString& error);

  // WebSocket discovery
  void discoverAndConnectWebSocket();

  // Connection status handlers (ConnectionManager)
  void onDaemonConnected();
  void onDaemonDisconnected();
  void updateConnectionStatus(ConnectionManager::ConnectionState state, ConnectionManager::ConnectionState oldState);
  void onConnectionStatusMessage(QString message, QString level);
  void onBlockchainSyncUpdate(int blocks, int headers);
  void maybeAutoStartDaemon();
  void onStartupWatchdogTimeout();  // #295: fail loud after 60s without RPC

private:
  void setupUI();
  bool startDaemonWithOptions(bool showFeedback, bool openLogWindow);
  bool maybeShowP2PNetworkNotice();
  void updateStatus(const QJsonObject& info);
  void updateEconomics(const QJsonObject& economics);
  void updateWallet(const QString& address);
  void updateExplorer(const QJsonValue& block);
  void displayAddressResult(const QJsonObject& result);
  void displayExplorerBlock(const QJsonObject& block, bool mainDetail = true);
  void displayExplorerTransaction(const QJsonObject& tx);
  void displayExplorerAddress();
  void updateExplorerRecentBlocks(int height);
  void requestNextExplorerRecentBlock();
  void requestExplorerBlockTransactions(const QJsonObject& block);
  bool handleExplorerRpcError(const QString& method, int code, const QString& message);
  void setExplorerStatus(const QString& text, bool warning = false);
  void resetExplorerDetailTables();
  void refreshAiStatusStrip();
  int livePeerCountForStatusStrip() const;
  void updateBridgeTab(const QJsonObject& stats);
  void updateMining(const QJsonObject& miningInfo);
  void updateMiningStats(const QJsonObject& miningInfo);
  void updateOverviewCpuTelemetry(const QJsonObject& cpuStats);
  void updateOverviewGpuTelemetry(const QJsonObject& gpuStats);
  void updateOverviewHardwareTelemetry();
  void setOverviewLocalHashrate(double hashrateHps, const QString& tooltip = QString());
  void setOverviewNetworkHashrate(double hashrateHps, const QString& tooltip = QString());
  void resetOverviewMiningTelemetry();
  void updateMiningReadinessDisplay(const QJsonObject& readiness);
  void resetMiningReadinessDisplay(const QString& summary, const QString& detail = QString());
  void updateNodeStatus(const QJsonObject& blockchainInfo, const QJsonObject& networkInfo, const QJsonObject& mempoolInfo);
  void updateNetworkInfo(const QJsonObject& networkInfo);
  QString networkDiagnosticsText() const;
  void updatePeerTable(const QJsonArray& peers);
  void updateBlockTemplate(const QJsonObject& blockTemplate);
  QString currentMiningMode() const;
  void setMiningModeControlsLocked(bool locked);
  void updateStratumIdentityLabel();
  void applyMiningFocusDim(bool enabled);
  void updateMiningFocusDimState();
  void setMiningOutputCinematicEnabled(bool enabled);
  void updateMiningOutputCinematicFrame();
  void updateWalletSwitcherState();
  bool shouldIgnoreWalletScopedResult(const QString& method) const;
  void bindWalletScopedState(const QString& walletName);
  void clearWalletScopedUiState();
  void refreshWalletMiningAddress();
  void startWalletRescan(const QString& statusText);
  bool isInputUtxoMissingError(const QString& errorText) const;
  void handleSpendInputMissing(const QString& errorText);
  QString currentWalletAddressMode() const;
  QString currentReceiveMode() const;
  QString currentSendMode() const;
  void updateWalletAddressModeUi();
  void updateReceiveModeUi();
  void applyPrimaryAddressesToReceiveTab();
  void updateSendModeUi();
  void updateWalletBalanceDisplay();
  void refreshShieldedBalanceSummary();
  void refreshConfidentialWalletState();
  bool collectSendForm(QString& recipient,
                       QString& amountText,
                       double& amount,
                       double& feeRate,
                       bool requireUnlocked);
  void startHardwareWalletSendFlow(const QString& recipient,
                                   const QString& amountText,
                                   double amount,
                                   double feeRate);
  void clearPendingHardwareWalletSend();
  void handleHardwareWalletBroadcast(const QString& txid, bool linkedSendFlow);

  RpcClient* rpc_;
  ChangeAddressManager* changeAddrMgr_;
  TransactionTracker* txTracker_;
  AdvisoryBannerQueue* bannerQueue_;
  QString activeReservationId_;  // Phase 6: tracks in-flight send reservation
  ConnectionManager* connectionMgr_;  // Bulletproof connection management
#ifdef DIN_EXPERIMENTAL_FEATURES
  WebSocketClient* ws_;
#endif
  QTimer* refreshTimer_;
  
  // Status bar
  QLabel* lblConnectionStatus_;
  QLabel* lblNetworkInfo_;       // Network chain (mainnet/testnet/regtest) + RPC/WS ports
  QLabel* lblDaemonVersion_;
  QLabel* lblDbHealth_;
  QLabel* lblErrorMessage_;      // Dedicated error message display at bottom
  
#ifdef HAVE_QT_QUICK
  // Mining pane (QML) - only if Qt Quick is available
  RpcHelper* rpcHelper_;
  QQuickWidget* miningWidget_;
#endif
  
  // Overview tab
  QLabel* lblHeight_;
  QLabel* lblHeaders_;
  QLabel* lblSyncProgress_;
  QLabel* lblConnections_;
  QLabel* lblMempool_;  // Overview mempool stats
  QLabel* lblPhase_;
  QLabel* lblSupply_;
  QLabel* lblReward_;

  // Monitoring Dashboard widgets (Overview bottom half)
  QProgressBar* cpuProgressBar_;
  QLabel* lblCpuUsage_;
  QLabel* lblCpuTemp_;
  QLabel* lblPowerStatus_;
  QLabel* lblLocalHashrate_;
  QLabel* lblNetworkHashrate_;
  QLabel* lblMinerModeOverview_;
  QLabel* lblGpuBackendOverview_;
  QLabel* lblGpuDeviceOverview_;
  QLabel* lblGpuLoadOverview_;
  QLabel* lblGpuMemoryOverview_;
  QLabel* lblGpuThermalsOverview_;
  QLabel* lblMempoolSize_;
  QLabel* lblMempoolBytes_;
  QLabel* lblPeersCount_;
  QLabel* lblPeersStatus_;
  QTableWidget* tblPeersOverview_;
  QTextEdit* txtAlerts_;

  // v7 Consensus Health (Overview tab)
  QLabel* lblUtreexoHealth_ = nullptr;
  QLabel* lblUtreexoLeaves_ = nullptr;
  QLabel* lblUtreexoRoot_ = nullptr;
  QLabel* lblPqOverviewRatio_ = nullptr;
  QLabel* lblPqOverviewUtxos_ = nullptr;
  QLabel* lblPqOverviewScheme_ = nullptr;

  // Explorer tab — Recent Blocks
  QTableWidget* tblRecentBlocks_ = nullptr;

  // Live Dashboard widgets
  QLabel* lblWsStatus_;  // WebSocket connection status
  QTableWidget* tblLiveEvents_;  // Live events feed (blocks + transactions)
  
  // Wallet tab
  QLineEdit* edtAddress_;
  QPushButton* btnNewAddress_;
  QPushButton* btnValidate_;
  QPushButton* btnCopy_;
  class QComboBox* cmbWalletAddressMode_ = nullptr;
  QLabel* lblReceivePathHint_ = nullptr;
  QLabel* lblBalance_;
  // v7 PQ health indicator
  QLabel* lblPqRatio_ = nullptr;
  class QProgressBar* barPqRatio_ = nullptr;
  QLabel* lblTotalWalletBalance_ = nullptr;
  QLabel* lblTransparentTaprootBalance_ = nullptr;
  QLabel* lblTransparentP2mrBalance_ = nullptr;
  QLabel* lblShieldedBalance_ = nullptr;
  QPushButton* btnConsolidate_ = nullptr;
  int cachedUtxoCount_ = 0;
  QJsonObject pendingConsolidateParams_;
  QTextEdit* txtValidation_;
  
  // Send tab — two-axis: Action (transfer/contract/convert) x Visibility (public/confidential/private)
  class QComboBox* cmbSendAction_ = nullptr;      // Step 1: what are you doing?
  class QComboBox* cmbSendMode_ = nullptr;         // Hidden: mirror of cmbSendAction_ ("public_transfer" / "public_contract")
  QLineEdit* edtRecipient_ = nullptr;
  QLineEdit* edtAmount_ = nullptr;
  QLineEdit* edtFee_ = nullptr;
  class QComboBox* cmbFeePreset_ = nullptr;
  QPushButton* btnSend_ = nullptr;
  QPushButton* btnHardwareWalletSend_ = nullptr;
  QPushButton* btnUseMax_ = nullptr;
  QLabel* lblSendStatus_ = nullptr;
  QLabel* lblEstimatedFee_ = nullptr;
  QTextEdit* txtSendResult_ = nullptr;
  HardwareWalletWidget* hardwareWalletWidget_ = nullptr;

  // Fee estimation cache (rc8): wallet.estimatefee response is stored here so
  // that collectSendForm() can apply the preset-driven rate to the actual send
  // call. Unit: una/vB (matching dinerod's wallet.estimatefee output and the
  // edtFee_ placeholder). 0.0 = not yet estimated; trigger an estimate before
  // sending.
  double currentEstimatedFeeRate_ = 0.0;
  int    currentEstimatedFeeBlocks_ = 0;

  // Contract template widgets (Phase 2: Private Contract path)
  QGroupBox* contractGroup_ = nullptr;
  class QComboBox* cmbContractTemplate_ = nullptr;
  class QStackedWidget* contractTemplateStack_ = nullptr;
  QWidget* contractVaultPage_ = nullptr;
  QWidget* contractConditionalPage_ = nullptr;  // Conditional Vault (CTV + CHECKSIG)
  QWidget* contractTimelockPage_ = nullptr;
  QWidget* contractPayrollPage_ = nullptr;
  QWidget* contractCustomPage_ = nullptr;
  QLineEdit* edtRecoveryPubkey_ = nullptr;      // Recovery key for conditional vault
  class QSpinBox* spnTimelockDuration_ = nullptr;
  class QComboBox* cmbTimelockUnit_ = nullptr;
  QTableWidget* tblPayrollRecipients_ = nullptr;
  QLabel* lblPayrollTotal_ = nullptr;
  QLineEdit* edtCustomScript_ = nullptr;
  QLabel* lblCustomScriptWarning_ = nullptr;

  // Contracts management tab (Phase 4)
  QTableWidget* tblContracts_ = nullptr;
  QLabel* lblContractsSummary_ = nullptr;
  bool pendingContractsRefresh_ = false;
  void refreshContractsList();
  void updateContractsTable(const QJsonValue& txList);

  struct PendingHardwareWalletSend {
    bool active = false;
    QString recipient;
    QString amountText;
    qint64 amountUna = 0;
    QStringList selectedInputOutpoints;
    QMap<QString, qint64> selectedInputAmountsUna;
    QString changeAddress;
    qint64 changeAmountUna = 0;
    qint64 feePaidUna = 0;
  } pendingHardwareWalletSend_;
  
  // Explorer tab
  QLineEdit* edtBlockHash_;
  QPushButton* btnGetBlock_;
  QTextEdit* txtBlockData_;
  QLabel* lblBestBlock_;
  QLabel* lblExplorerSummary_;
  QTableWidget* tblExplorerUTXOs_;
  QLabel* lblExplorerStatus_ = nullptr;
  QLabel* lblExplorerHeight_ = nullptr;
  QLabel* lblExplorerDifficulty_ = nullptr;
  QLabel* lblExplorerHashrate_ = nullptr;
  QLabel* lblExplorerSupply_ = nullptr;
  QTableWidget* tblExplorerTransactions_ = nullptr;
  QTableWidget* tblExplorerInputs_ = nullptr;
  QTableWidget* tblExplorerOutputs_ = nullptr;
  QVector<int> pendingExplorerRecentHeights_;
  QMap<QString, int> explorerRecentBlockRows_;
  QMap<QString, int> explorerBlockTransactionRows_;
  int pendingExplorerRecentHeight_ = -1;
  int explorerLatestBlocksHeight_ = -1;
  QString pendingExplorerHeightLookup_;
  QString pendingExplorerBlockHash_;
  QString pendingExplorerTxLookup_;
  QString pendingExplorerTxFallbackBlockHash_;
  bool pendingExplorerTxAliasTried_ = false;
  QString pendingExplorerAddress_;
  QJsonValue pendingExplorerAddressBalance_;
  QJsonValue pendingExplorerAddressHistory_;
  bool pendingExplorerAddressBalanceReady_ = false;
  bool pendingExplorerAddressHistoryReady_ = false;
  bool explorerAddressBalanceAliasTried_ = false;
  bool explorerAddressHistoryAliasTried_ = false;
  bool explorerAddressScantxFallbackTried_ = false;
  
  // Mining tab
  QLabel* lblMiningPhase_;
  QLabel* lblNextReward_;
  QLabel* lblDifficulty_;
  // lblMempool_ moved to Overview tab (see above)
  QTextEdit* txtMiningInfo_;
  
  // Mining controls
  class QComboBox* cmbMiningMode_ = nullptr; // Solo (default) vs Pool (Stratum)
  QLabel* lblMinerType_ = nullptr;
  class QComboBox* cmbMinerType_;  // Solo miner engine selection
  QLabel* lblMinerPath_ = nullptr;
  QLineEdit* edtMinerPath_;        // Path to process-based miner/worker binary
  QPushButton* btnBrowseMiner_;    // Browse for miner/worker binary
  QLabel* lblStratumEndpoint_ = nullptr;
  QLineEdit* edtStratumEndpoint_ = nullptr;
  QPushButton* btnLocalStratum_ = nullptr;
  QLabel* lblSv2Endpoint_ = nullptr;
  QLineEdit* edtSv2Endpoint_ = nullptr;
  QLabel* lblSv2Pubkey_ = nullptr;
  QLineEdit* edtSv2Pubkey_ = nullptr;
  QLabel* lblSv2Backend_ = nullptr;
  class QComboBox* cmbSv2Backend_ = nullptr;
  QLabel* lblSv2RewardMode_ = nullptr;
  class QComboBox* cmbSv2RewardMode_ = nullptr;
  QLabel* lblSv2Shares_ = nullptr;  // Live "Shares: seq=… total=…" in SV2 mode
  QLineEdit* edtMiningAddress_;
  QLabel* lblStratumIdentity_;
  QLineEdit* edtMiningThreads_;
  QPushButton* btnStartMining_;  // v0.14.0.4: Single toggle button (Start ↔ Stop)
  QPushButton* btnStopMining_;   // Legacy - kept for compatibility, hidden in UI
  QPushButton* btnUseWalletAddr_;  // "Use Wallet" button for mining address
  QLabel* lblMiningStatus_ = nullptr;
  QLabel* lblMiningReadiness_ = nullptr;
  QLabel* lblHashrate_;
  bool isMining_ = false;  // v0.14.0.4: Track mining state for toggle button
  bool externalMinerStopRequested_ = false;
  QString activeMinerType_ = "none";  // "internal", "stratum_worker", "external", "gpu", "daemon", or "none"

public:
  // Public accessors for the Cmd+K dashboard. Read-only snapshot of the
  // qt-app-side state the daemon doesn't expose.
  bool    isMiningLocal()     const { return isMining_; }
  QString activeMinerType()   const { return activeMinerType_; }
  double  currentHashrate()   const { return mining_stats_.current_hashrate; }
  // Wall-clock seconds since this MainWindow was constructed. Daemon has no
  // getuptime method on this build, so the dashboard uses app uptime as the
  // user-visible "I've been on the network for X" signal.
  qint64  appUptimeSeconds()  const;
private:
  qint64  app_started_at_ms_ = 0;  // set in ctor
  QTextEdit* txtMiningOutput_;
  QTabWidget* mainTabs_ = nullptr;
  QWidget* miningTabWidget_ = nullptr;
  QWidget* miningInfoGroup_ = nullptr;
  QWidget* miningControlsGroup_ = nullptr;
  QLabel* lblMiningOutputSection_ = nullptr;
  QGraphicsColorizeEffect* miningInfoDimEffect_ = nullptr;
  QGraphicsColorizeEffect* miningControlsDimEffect_ = nullptr;
  QGraphicsColorizeEffect* miningOutputLabelDimEffect_ = nullptr;
  bool miningFocusDimApplied_ = false;

  // Embedded miner (in-process via dinero-solo-miner library)
  MinerController* minerCtrl_;

  // Mining process (external mode only)
  class QProcess* miningProcess_;
  class QProcess* localStratumProcess_ = nullptr;

  // Daemon process (if started by GUI)
  class QProcess* daemonProcess_;
  bool suppressErrorDialogs_;  // Suppress error dialogs during daemon startup
  bool autoStartDaemonAttempted_ = false;
  bool daemonStopRequested_ = false;  // #295: user pressed Stop Daemon — suppress exit dialog
  int daemonLaunchRetries_ = 0;  // auto-retry transient launch races (datadir lock / port held by a slow-exiting prior daemon) before showing the failure dialog

  // Debug console - live log viewer
  dinero::DebugConsole* debugConsole_;

  // DPI Pay/Collect widget
  DpiWidget* dpiWidget_ = nullptr;

  // Bridge and Payments widgets
#ifdef DIN_EXPERIMENTAL_FEATURES
  // BridgeWidget* bridgeWidget_;  // DISABLED - not ready for production
  PaymentsWidget* paymentsWidget_;
  EscrowWidget* escrowWidget_;
  MarketplaceWidget* marketplaceWidget_;
#endif

  // Track C Liquidity Vault — production, not experimental.
  class VaultPanel* vaultPanel_ = nullptr;
  class PoolPanel* poolPanel_ = nullptr;

  // Phase 5 Shielded pool surface. Held as a member so the wallet-switch
  // path can poke it for an immediate refresh; otherwise it would only
  // re-fetch on its 6s tip-poll timer and display stale balance/notes.
  class ShieldedWidget* shieldedWidget_ = nullptr;

  // NOTE: Lightning Network moved to separate lightning-main branch (L2)
  // dinero::LightningWidget* lightningWidget_;  // REMOVED for L1 purity

  // Block found image display
  QLabel* lblBlockFoundImage_;
  
  // Mining statistics tracking
  struct MiningStats {
    int blocks_found = 0;
    qint64 mining_started = 0; // QDateTime::currentMSecsSinceEpoch()
    double current_hashrate = 0.0;
    qint64 total_hashes = 0;
    QList<double> hashrate_samples; // Recent samples for averaging
    // SV2-specific: session-cumulative accepted share count (pool's
    // per-submit accepted_count field is only 1 per ack).
    qint64 sv2_shares_accepted = 0;
    qint64 sv2_hashrate_updates = 0;
    qint64 sv2_window_bps = -1;
    qint64 sv2_window_shares = 0;
  };
  MiningStats mining_stats_;
  
  // Mining statistics UI
  QLabel* lblBlocksFound_ = nullptr;
  QLabel* lblMiningUptimeCaption_ = nullptr;
  QLabel* lblMiningUptime_ = nullptr;
  QLabel* lblCurrentHash_ = nullptr;
  QLabel* lblTotalHashes_ = nullptr;
  
  QTimer* miningStatsTimer_; // Update stats every second
  QTimer* miningFocusDimTimer_; // Delayed dimming when mining tab is active
  QTimer* miningCinematicTimer_; // Cinematic matrix background while mining
  int miningCinematicFrame_ = 0;
  int miningCinematicLastLongCometFrame_ = -100000;
  int miningCinematicLastUltraCometFrame_ = 0;
  struct MiningCinematicSpark {
    qint64 worldRow = 0;
    int col = 0;
    int bornFrame = 0;
    int lifetimeFrames = 0;
    int streakLength = 1; // 1 = single spark glyph; >1 = short comet streak.
  };
  QVector<MiningCinematicSpark> miningCinematicSparks_;
  QLabel* mottoTickerLabel_ = nullptr;
  void updateMiningStats();
  void updateMiningRuntimeLabel();
  void updateMiningStatsRPC();  // Phase Y: Poll integrated CPU miner
  void handleExternalMinerStats(const QJsonObject& stats);  // Display verifiable protocol events
  void parseMiningOutput(const QString& line);

  // Image-based mining status helpers
  void showBlockFoundImage();
  void showBlockRejectedImage();
  QPixmap createRejectedImage(const QPixmap& baseImage);
  
  // Daemon control
  QPushButton* btnStartDaemon_;
  QPushButton* btnStopDaemon_;
  QLineEdit* edtDaemonPath_;        // Path to dinerod binary
  QPushButton* btnBrowseDaemon_;    // Browse for dinerod binary

  // Wallet lock/unlock (single toggle button with status in text)
  QPushButton* btnWalletLock_;      // Shows status + toggles Lock/Unlock
  QPushButton* btnEncryptWallet_;
  class QComboBox* cmbWalletSelector_;
  QPushButton* btnLoadWallet_;
  QPushButton* btnRescanWallet_;
  QLabel* lblWalletName_;           // Shows active wallet name
  bool singleWalletMode_;
  bool autoLoadDefaultAttempted_;
  bool walletUnlocked_;
  bool walletRescanning_;           // Track if blockchain rescan is active
  bool walletReorgInfoSupported_ = true;
  bool walletSwitchInFlight_;
  QString pendingWalletOpenName_;
  bool shuttingDown_;
  QTimer* safeModeRescanRetryTimer_;
  int safeModeRescanRetryAttempts_;
  bool safeModeRescanRetryScheduled_;
  QTimer* unlockCountdownTimer_;    // Timer for unlock countdown display
  int unlockSecondsRemaining_;      // Seconds until auto-lock
  QString currentWalletName_;       // Name of current wallet

  // Primary addresses cached from wallet.getinfo (deterministic from seed)
  QString cachedPrimaryAddress_;         // Transparent (din1p / din1r)

  // Receive tab (address list)
  QTableWidget* tblAddresses_;
  QPushButton* btnDeriveAddress_;
  class QComboBox* cmbReceiveMode_ = nullptr;
  QPushButton* btnLoadAllAddresses_ = nullptr;
  class QCheckBox* chkHideZeroBalance_;
  void onAddressLabelDoubleClicked(int row, int column);
  void onAddressLabelChanged(QTableWidgetItem* item);
  bool labelEditInProgress_;  // Prevent re-entrancy during label save

  // Transactions tab (history)
  QTableWidget* tblTransactions_;
  class QComboBox* cmbTxTypeFilter_ = nullptr;
  void loadTransactionHistory();
  void updateTransactionTable(const QJsonArray& transactions);
  
  // UTXOs tab
  void updateUTXOTable(const QJsonArray& utxos);

  // Network verification
  QLabel* lblNetworkWarning_;  // Warning banner for wrong network
  QString expectedGenesisHash_;  // Production genesis hash
  QString detectedChain_;  // Detected network chain name (main/test/regtest)
  bool networkVerified_;  // Track if network has been verified

  // Node Status Pill (getblockchaininfo, getnetworkinfo, getmempoolinfo)
  QLabel* lblNodeChain_;        // Chain name (main/test/regtest)
  QLabel* lblNodeHeight_;       // Current block height
  QLabel* lblNodePeers_;        // Peer count
  QLabel* lblNodeMempool_;      // Mempool tx count
  QLabel* lblNodeSyncStatus_;   // Sync status (synced/syncing)

  // Peer Table tab
  QTableWidget* tblPeers_;      // Peer list table
  QPushButton* btnRefreshPeers_;
  QPushButton* btnDisconnectPeer_;
  QPushButton* btnBanPeer_;
  QPushButton* btnReconnectAllPeers_;
  QPushButton* btnCopyNetworkDiagnostics_;
  QLabel* lblPeerSummary_;
  QLabel* lblPeerReachability_;
  QLabel* lblPeerPortMapping_;
  QLabel* lblPeerRelay_;
  QLabel* lblPeerAdvertised_;
  QLabel* lblPeerReachabilityAdvice_;

  // Block Template Viewer tab
  QTextEdit* txtBlockTemplate_; // Block template JSON display
  QPushButton* btnRefreshTemplate_;
  QLabel* lblTemplateHeight_;
  QLabel* lblTemplateTxCount_;
  QLabel* lblTemplateFees_;
  QLabel* lblTemplateDifficulty_;

  // QR Code generator
  QLabel* lblQRCode_;           // QR code image display
  QPushButton* btnGenerateQR_;
  QLineEdit* edtQRAddress_;     // Address to encode

  // Address book (for CSV import/export)
  QPushButton* btnImportCSV_;
  QPushButton* btnExportCSV_;

  // Daemon log file monitoring
  QFileSystemWatcher* daemonLogWatcher_;
  QString daemonLogPath_;
  qint64 lastDaemonLogPos_;
  void onDaemonLogChanged();
  void readNewDaemonLogLines();

  // Utreexo proof-service diagnostics tab
  QLabel* lblBridgeStatus_ = nullptr;
  QLabel* lblBridgeSummary_ = nullptr;
  QLabel* lblBridgeRequests_ = nullptr;
  QLabel* lblBridgeQueue_ = nullptr;
  QLabel* lblBridgeCacheHits_ = nullptr;
  QLabel* lblBridgeCacheMisses_ = nullptr;
  QLabel* lblBridgeHitRate_ = nullptr;
  QLabel* lblBridgeBlockEntries_ = nullptr;
  QLabel* lblBridgeTxEntries_ = nullptr;
  QLabel* lblBridgeIndexed_ = nullptr;
  QLabel* lblBridgeEvictions_ = nullptr;
  QLabel* lblBridgeWorkers_ = nullptr;
  QLabel* lblBridgeActiveGens_ = nullptr;
  QLabel* lblBridgeLatency_ = nullptr;
  QLabel* lblBridgeQueueWait_ = nullptr;
  QLabel* lblBridgePriority_ = nullptr;
  QLabel* lblBridgeTasks_ = nullptr;

  // AI Assistant
  AiPanel* aiPanel_ = nullptr;
  dinero::qt::dashboard::CmdKPanel* cmdKPanel_ = nullptr;
  AiStatusStrip* aiStatusStrip_ = nullptr;
  QShortcut* aiToggleShortcut_ = nullptr;
  void onToggleAiPanel();

  // Cached RPC state for AI status strip
  int cachedHeight_ = 0;
  int cachedHeaders_ = 0;
  int cachedPeerCount_ = 0;
  QJsonObject cachedNetworkInfo_;
  double cachedBalance_ = 0.0;
  double cachedTransparentBalance_ = 0.0;
  double cachedP2mrBalance_ = 0.0;
  double cachedShieldedBalance_ = 0.0;
  double cachedPendingBalance_ = 0.0;
  double cachedMiningBalance_ = 0.0;
  bool cachedMiningActive_ = false;
  QString lastMiningReadinessFingerprint_;
  double cachedHashrate_ = 0.0;
  double cachedNetworkHashrate_ = 0.0;
  bool cachedBridgeActive_ = false;
  int cachedProofCacheEntries_ = 0;
  bool overviewGpuStatsAvailable_ = false;
  double overviewGpuDeviceUtilization_ = 0.0;
  double overviewGpuRendererUtilization_ = 0.0;
  double overviewGpuTilerUtilization_ = 0.0;
  qint64 overviewGpuMemoryInUse_ = 0;
  qint64 overviewGpuMemoryAllocated_ = 0;
  bool overviewGpuTempAvailable_ = false;
  double overviewGpuTempC_ = 0.0;
  bool overviewGpuFanAvailable_ = false;
  qint64 overviewGpuFanRpm_ = 0;
  QString overviewGpuThermalReason_;
  QString overviewGpuBackend_;
  QString overviewGpuDevice_;
  int overviewGpuDeviceCount_ = 0;
  bool overviewGpuTelemetrySeen_ = false;
};
