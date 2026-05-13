# Dinero-Qt Implementation Plan: Following Bitcoin-Qt Architecture

## Current State Analysis

### What Dinero Does Well ✅

1. **Node Interface Abstraction**: `src/node/interfaces.h` provides a clean abstraction layer
2. **Embedded Architecture**: `dinero-embedded-qt6` already follows Bitcoin-Qt pattern
3. **Background Thread Initialization**: `InitExecutor` properly handles node startup
4. **Model-View Separation**: `ClientModel` and `WalletModel` provide clean data layers

### Current Issues 🔧

1. **Multiple GUI Architectures**: Three different approaches instead of one unified approach
2. **HTTP RPC Overhead**: GUI apps use HTTP RPC instead of in-process calls
3. **Subprocess Dependencies**: Some GUIs spawn `dinero-cli` processes
4. **Missing In-Process RPC**: No direct RPC execution without HTTP overhead

## Implementation Plan

### Phase 1: Enhance Node Interface for In-Process RPC

#### 1.1 Add Direct RPC Execution to Node Interface

```cpp
// src/node/interfaces.h - Add these methods
class Node {
public:
    // Direct RPC execution (no HTTP overhead)
    virtual std::string executeRpc(const std::string& command) = 0;
    virtual std::string executeRpc(const std::string& method, 
                                  const std::vector<std::string>& params) = 0;
    virtual Json::Value executeRpcJson(const std::string& method,
                                      const Json::Value& params) = 0;
    
    // Batch RPC execution for efficiency
    virtual std::vector<Json::Value> executeRpcBatch(
        const std::vector<std::pair<std::string, Json::Value>>& requests) = 0;
};
```

#### 1.2 Implement In-Process RPC in NodeImpl

```cpp
// src/node/node_impl.cpp - Add implementation
std::string NodeImpl::executeRpc(const std::string& command) {
    if (!m_rpc_server || !m_running.load()) {
        throw std::runtime_error("Node not running or RPC server not available");
    }
    
    // Parse command into method and params
    // This is similar to bitcoin-cli parsing but in-process
    std::string method;
    Json::Value params;
    parseRpcCommand(command, method, params);
    
    return executeRpcJson(method, params).toStyledString();
}

Json::Value NodeImpl::executeRpcJson(const std::string& method, const Json::Value& params) {
    if (!m_rpc_server || !m_running.load()) {
        Json::Value error;
        error["code"] = -1;
        error["message"] = "Node not running";
        return error;
    }
    
    // Call RPC method directly without HTTP overhead
    try {
        return m_rpc_server->callMethod(method, params);
    } catch (const std::exception& e) {
        Json::Value error;
        error["code"] = -32603;
        error["message"] = e.what();
        return error;
    }
}
```

### Phase 2: Create Unified Qt Application

#### 2.1 Design Unified Main Window

```cpp
// src/qt/mainwindow.h
class MainWindow : public QMainWindow {
    Q_OBJECT
    
public:
    enum class ConnectionMode {
        Embedded,    // Default: embedded node (Bitcoin-Qt style)
        Remote       // Optional: connect to remote node
    };
    
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    
    void setConnectionMode(ConnectionMode mode, const QString& host = "", int port = 0);
    
private slots:
    void onNodeReady();
    void onNodeError(const QString& error);
    void showConnectionDialog();
    void showDebugConsole();
    
private:
    void setupUi();
    void setupMenus();
    void startEmbeddedNode();
    void connectToRemoteNode(const QString& host, int port);
    void connectModelsToUi();
    void enableInterface();
    
    // Node management
    ConnectionMode m_connection_mode = ConnectionMode::Embedded;
    std::unique_ptr<dinero::interfaces::Node> m_node;
    std::unique_ptr<RpcClient> m_rpc_client;  // For remote connections
    
    // Models
    ClientModel* m_client_model = nullptr;
    WalletModel* m_wallet_model = nullptr;
    
    // UI components
    QTabWidget* m_tab_widget;
    QWidget* m_overview_tab;
    QWidget* m_send_tab;
    QWidget* m_receive_tab;
    QWidget* m_transactions_tab;
    QWidget* m_mining_tab;
    
    // Background initialization
    NodeRunner* m_node_runner = nullptr;
    QSplashScreen* m_splash_screen = nullptr;
};
```

#### 2.2 Implement Connection Mode Switching

```cpp
// src/qt/mainwindow.cpp
void MainWindow::setConnectionMode(ConnectionMode mode, const QString& host, int port) {
    if (m_connection_mode == mode) return;
    
    // Stop current connection
    if (m_node && m_node->isRunning()) {
        m_node->stop();
    }
    if (m_rpc_client) {
        m_rpc_client.reset();
    }
    
    m_connection_mode = mode;
    
    if (mode == ConnectionMode::Embedded) {
        startEmbeddedNode();
    } else {
        connectToRemoteNode(host, port);
    }
}

void MainWindow::startEmbeddedNode() {
    // Create embedded node (Bitcoin-Qt style)
    m_node = dinero::interfaces::MakeNode();
    
    // Configure node options from command line args
    dinero::interfaces::NodeInitOptions options;
    configureNodeOptions(options);
    
    // Start node in background thread
    m_node_runner = new NodeRunner(m_node.get(), options, this);
    
    connect(m_node_runner, &NodeRunner::initMessage, 
            this, &MainWindow::showStatusMessage);
    connect(m_node_runner, &NodeRunner::ready, 
            this, &MainWindow::onNodeReady);
    connect(m_node_runner, &NodeRunner::fatalError, 
            this, &MainWindow::onNodeError);
    
    m_node_runner->start();
}
```

### Phase 3: Implement In-Process Debug Console

#### 3.1 Create Bitcoin-Qt Style RPC Console

```cpp
// src/qt/rpcconsole.h
class RpcConsole : public QWidget {
    Q_OBJECT
    
public:
    explicit RpcConsole(dinero::interfaces::Node* node, QWidget* parent = nullptr);
    
    void setNode(dinero::interfaces::Node* node);
    
private slots:
    void executeCommand();
    void clearConsole();
    void browseHistory(int offset);
    
private:
    void setupUi();
    void addToHistory(const QString& command);
    void showResult(const QString& command, const QString& result);
    void showError(const QString& command, const QString& error);
    
    dinero::interfaces::Node* m_node;
    QTextEdit* m_output;
    QLineEdit* m_input;
    QPushButton* m_execute_button;
    QPushButton* m_clear_button;
    
    QStringList m_command_history;
    int m_history_index = -1;
};

// src/qt/rpcconsole.cpp
void RpcConsole::executeCommand() {
    QString command = m_input->text().trimmed();
    if (command.isEmpty()) return;
    
    addToHistory(command);
    m_output->append(QString("<b>&gt; %1</b>").arg(command.toHtmlEscaped()));
    
    try {
        if (m_node && m_node->isRunning()) {
            // Execute in-process (Bitcoin-Qt style - no HTTP overhead)
            QString result = QString::fromStdString(
                m_node->executeRpc(command.toStdString())
            );
            showResult(command, result);
        } else {
            showError(command, "Node not running");
        }
    } catch (const std::exception& e) {
        showError(command, QString("Error: %1").arg(e.what()));
    }
    
    m_input->clear();
    m_history_index = -1;
}
```

### Phase 4: Eliminate Subprocess Dependencies

#### 4.1 Replace RpcClient with Direct Node Calls

```cpp
// Current problematic pattern (avoid this):
void MainWindow::onGetNewAddress() {
    QJsonArray params; 
    params << "" << "bech32";
    m_rpc_client->call("getnewaddress", params);  // HTTP overhead
}

// Bitcoin-Qt style pattern (use this):
void MainWindow::onGetNewAddress() {
    if (!m_node || !m_node->isRunning()) {
        setStatus("Node not running", "#f87171");
        return;
    }
    
    try {
        // Direct in-process call (no HTTP overhead)
        std::string address = m_node->getNewAddress("bech32");
        m_last_address_label->setText(QString::fromStdString(address));
        setStatus("Address generated ✓", "#22c55e");
    } catch (const std::exception& e) {
        setStatus(QString("Error: %1").arg(e.what()), "#f87171");
    }
}
```

#### 4.2 Add High-Level Methods to Node Interface

```cpp
// src/node/interfaces.h - Add convenience methods
class Node {
public:
    // High-level wallet operations (no RPC parsing overhead)
    virtual std::string getNewAddress(const std::string& addressType = "bech32") = 0;
    virtual double getBalance() const = 0;
    virtual std::vector<Transaction> getTransactions(int limit = 100) = 0;
    virtual std::string sendToAddress(const std::string& address, double amount) = 0;
    
    // High-level mining operations
    virtual bool startMining(int threads = 1) = 0;
    virtual bool stopMining() = 0;
    virtual MiningStats getMiningStats() const = 0;
    virtual std::vector<std::string> generateToAddress(int blocks, const std::string& address) = 0;
    
    // High-level blockchain operations
    virtual BlockchainInfo getBlockchainInfo() const = 0;
    virtual Block getBlock(const std::string& hash) const = 0;
    virtual std::string getBlockHash(int height) const = 0;
};
```

### Phase 5: Unified Application Structure

#### 5.1 Single Application with Multiple Modes

```cpp
// main.cpp - Unified entry point
int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    
    // Initialize OpenSSL providers
    init_openssl_providers();
    
    // Parse command line arguments
    CommandLineArgs args = parseCommandLine(app.arguments());
    
    // Create main window
    MainWindow window;
    
    // Configure connection mode
    if (args.remote_host.isEmpty()) {
        // Default: embedded mode (Bitcoin-Qt style)
        window.setConnectionMode(MainWindow::ConnectionMode::Embedded);
    } else {
        // Remote mode (optional)
        window.setConnectionMode(MainWindow::ConnectionMode::Remote, 
                               args.remote_host, args.remote_port);
    }
    
    window.show();
    return app.exec();
}
```

#### 5.2 Application Modes

| Mode | Description | Use Case |
|------|-------------|----------|
| **Embedded** (Default) | Runs node in background thread | Most users, Bitcoin-Qt style |
| **Remote** | Connects to external node | Advanced users, server setups |
| **Lite** | Connects to trusted node | Mobile/resource-constrained |

### Phase 6: Build System Updates

#### 6.1 CMake Target Consolidation

```cmake
# CMakeLists.txt - Unified Qt application
add_executable(dinero-qt
    src/qt/main.cpp
    src/qt/mainwindow.cpp
    src/qt/rpcconsole.cpp
    src/qt/clientmodel.cpp
    src/qt/walletmodel.cpp
    src/qt/initexecutor.cpp
    # ... other Qt sources
)

target_link_libraries(dinero-qt
    dinero_node          # Embedded node functionality
    dinero_common        # Common utilities
    dinero_primitives    # Blockchain primitives
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
)

# Optional: Keep separate tools for advanced users
add_executable(dinerod src/daemon/main.cpp)  # Headless daemon
add_executable(dinero-cli src/cli/main.cpp)  # RPC client tool
```

## Migration Strategy

### Step 1: Enhance Existing Embedded GUI ✅
- `dinero-embedded-qt6` is already close to Bitcoin-Qt pattern
- Add in-process RPC execution
- Enhance Node interface with direct methods

### Step 2: Create Unified Application
- Merge best features from all GUI applications
- Implement connection mode switching
- Add comprehensive debug console

### Step 3: Deprecate Old Applications
- Mark `dinero-qt6` (HTTP-only) as deprecated
- Mark `dinero-cli-gui-qt6` (subprocess) as deprecated
- Provide migration guide for users

### Step 4: Testing and Validation
- Ensure embedded mode works identically to current daemon
- Verify remote mode maintains compatibility
- Test all RPC methods work in-process

## Benefits of This Approach

### For Users
- **Single Application**: No confusion about which GUI to use
- **Better Performance**: In-process calls eliminate HTTP overhead
- **Immediate Feedback**: No network delays for local operations
- **Consistent Experience**: Same interface regardless of connection mode

### For Developers
- **Simpler Codebase**: One GUI application instead of three
- **Easier Debugging**: Direct method calls, no HTTP parsing
- **Better Testing**: Can test GUI and node together
- **Cleaner Architecture**: Follows proven Bitcoin-Qt pattern

### For Deployment
- **Single Binary**: Easier packaging and distribution
- **Fewer Dependencies**: No need for separate daemon process
- **Better Integration**: GUI and node share same process space
- **Consistent Behavior**: Same code paths as headless daemon

## Implementation Timeline

| Phase | Duration | Deliverables |
|-------|----------|--------------|
| **Phase 1** | 1 week | Enhanced Node interface with in-process RPC |
| **Phase 2** | 2 weeks | Unified MainWindow with connection modes |
| **Phase 3** | 1 week | In-process debug console |
| **Phase 4** | 1 week | Remove subprocess dependencies |
| **Phase 5** | 1 week | Unified application structure |
| **Phase 6** | 1 week | Build system updates and testing |

**Total: 7 weeks to complete Bitcoin-Qt style architecture**

## Success Metrics

- ✅ Single `dinero-qt` application replaces three GUI apps
- ✅ In-process RPC calls eliminate HTTP overhead
- ✅ Debug console executes commands without network calls
- ✅ Embedded mode provides same functionality as `dinerod`
- ✅ Remote mode maintains compatibility with existing setups
- ✅ No subprocess dependencies in GUI applications
- ✅ Build system produces clean, minimal binaries

This plan transforms Dinero into a true Bitcoin-Qt style application while maintaining all existing functionality and improving performance, user experience, and code maintainability.
