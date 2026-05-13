/**
 * Example: Bitcoin-Qt Style RPC Console for Dinero
 * 
 * This demonstrates how to create an in-process RPC console that executes
 * commands directly without HTTP overhead, following Bitcoin-Qt patterns.
 */

#include <QApplication>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QSplitter>
#include <QTimer>
#include <QThread>
#include <QMessageBox>
#include <QKeyEvent>

#include "node/interfaces.h"
#include "qt/initexecutor.h"

class RpcConsole : public QWidget {
    Q_OBJECT
    
public:
    explicit RpcConsole(dinero::interfaces::Node* node, QWidget* parent = nullptr)
        : QWidget(parent), m_node(node) {
        setupUi();
        setupConnections();
    }
    
private slots:
    void executeCommand() {
        QString command = m_input->text().trimmed();
        if (command.isEmpty()) return;
        
        // Add to history
        m_command_history.prepend(command);
        if (m_command_history.size() > 100) {
            m_command_history.removeLast();
        }
        m_history_index = -1;
        
        // Show command in output
        m_output->append(QString("<b>&gt; %1</b>").arg(command.toHtmlEscaped()));
        
        try {
            if (m_node && m_node->isRunning()) {
                // Execute in-process (Bitcoin-Qt style - no HTTP overhead)
                QString result = QString::fromStdString(
                    m_node->executeRpc(command.toStdString())
                );
                
                // Pretty print JSON result
                Json::Json::Value json_result;
                try { json_result = Json::Reader().parse(result.toStdString(, result)); } catch(...) { /* parse failed */ } {
                    Json::StyledWriter writer;
                    QString formatted = QString::fromStdString(writer.write(json_result));
                    m_output->append(formatted);
                } else {
                    m_output->append(result);
                }
            } else {
                m_output->append("<span style='color: red;'>Error: Node not running</span>");
            }
        } catch (const std::exception& e) {
            m_output->append(QString("<span style='color: red;'>Error: %1</span>")
                           .arg(QString::fromStdString(e.what()).toHtmlEscaped()));
        }
        
        m_input->clear();
        
        // Scroll to bottom
        QTextCursor cursor = m_output->textCursor();
        cursor.movePosition(QTextCursor::End);
        m_output->setTextCursor(cursor);
    }
    
    void clearConsole() {
        m_output->clear();
        m_output->append("<b>Dinero RPC Console</b>");
        m_output->append("Type 'help' for available commands.");
        m_output->append("");
    }
    
    void browseHistory(int offset) {
        if (m_command_history.isEmpty()) return;
        
        m_history_index += offset;
        if (m_history_index < 0) {
            m_history_index = 0;
        } else if (m_history_index >= m_command_history.size()) {
            m_history_index = m_command_history.size() - 1;
        }
        
        if (m_history_index >= 0 && m_history_index < m_command_history.size()) {
            m_input->setText(m_command_history[m_history_index]);
        }
    }
    
protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Up) {
            browseHistory(1);
            event->accept();
        } else if (event->key() == Qt::Key_Down) {
            browseHistory(-1);
            event->accept();
        } else {
            QWidget::keyPressEvent(event);
        }
    }
    
private:
    void setupUi() {
        setWindowTitle("Dinero RPC Console");
        resize(800, 600);
        
        auto* layout = new QVBoxLayout(this);
        
        // Output area
        m_output = new QTextEdit();
        m_output->setReadOnly(true);
        m_output->setFont(QFont("Consolas", 10));
        layout->addWidget(m_output);
        
        // Input area
        auto* input_layout = new QHBoxLayout();
        
        input_layout->addWidget(new QLabel("&gt;"));
        
        m_input = new QLineEdit();
        m_input->setFont(QFont("Consolas", 10));
        input_layout->addWidget(m_input);
        
        m_execute_button = new QPushButton("Execute");
        input_layout->addWidget(m_execute_button);
        
        m_clear_button = new QPushButton("Clear");
        input_layout->addWidget(m_clear_button);
        
        layout->addLayout(input_layout);
        
        // Initial content
        clearConsole();
        
        // Focus on input
        m_input->setFocus();
    }
    
    void setupConnections() {
        connect(m_execute_button, &QPushButton::clicked, this, &RpcConsole::executeCommand);
        connect(m_clear_button, &QPushButton::clicked, this, &RpcConsole::clearConsole);
        connect(m_input, &QLineEdit::returnPressed, this, &RpcConsole::executeCommand);
    }
    
private:
    dinero::interfaces::Node* m_node;
    QTextEdit* m_output;
    QLineEdit* m_input;
    QPushButton* m_execute_button;
    QPushButton* m_clear_button;
    
    QStringList m_command_history;
    int m_history_index = -1;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
    
public:
    MainWindow(QWidget* parent = nullptr) : QMainWindow(parent) {
        setupUi();
        startNode();
    }
    
private slots:
    void onNodeReady() {
        m_status_label->setText("Node: Running ✓");
        m_status_label->setStyleSheet("color: green;");
        
        // Enable RPC console
        m_rpc_console->setEnabled(true);
        
        // Show some initial info
        showNodeInfo();
    }
    
    void onNodeError(const QString& error) {
        m_status_label->setText("Node: Error");
        m_status_label->setStyleSheet("color: red;");
        
        QMessageBox::critical(this, "Node Error", 
                            "Failed to start node:\n\n" + error);
    }
    
private:
    void setupUi() {
        setWindowTitle("Dinero Bitcoin-Qt Style Demo");
        resize(1000, 700);
        
        auto* central = new QWidget();
        setCentralWidget(central);
        
        auto* layout = new QVBoxLayout(central);
        
        // Status bar
        m_status_label = new QLabel("Node: Starting...");
        layout->addWidget(m_status_label);
        
        // RPC Console
        m_rpc_console = new RpcConsole(nullptr);  // Will set node later
        m_rpc_console->setEnabled(false);
        layout->addWidget(m_rpc_console);
    }
    
    void startNode() {
        // Create embedded node (Bitcoin-Qt style)
        m_node = dinero::interfaces::MakeNode();
        
        // Configure node options
        dinero::interfaces::NodeInitOptions options;
        options.testnet = false;
        options.regtest = true;  // Use regtest for demo
        options.adjustForNetwork();
        
        // Start node in background thread
        m_init_executor = new InitExecutor(m_node.get(), this);
        m_init_thread = new QThread(this);
        m_init_executor->moveToThread(m_init_thread);
        
        connect(m_init_thread, &QThread::started, [this, options]() {
            m_init_executor->start(options);
        });
        
        connect(m_init_executor, &InitExecutor::finished, this, 
                [this](bool success, const QString& error) {
                    m_init_thread->quit();
                    m_init_executor->deleteLater();
                    m_init_thread->deleteLater();
                    
                    if (success) {
                        // Set node in RPC console
                        m_rpc_console->setParent(nullptr);
                        delete m_rpc_console;
                        m_rpc_console = new RpcConsole(m_node.get(), this);
                        
                        // Add to layout
                        auto* layout = qobject_cast<QVBoxLayout*>(centralWidget()->layout());
                        layout->addWidget(m_rpc_console);
                        
                        onNodeReady();
                    } else {
                        onNodeError(error);
                    }
                });
        
        m_init_thread->start();
    }
    
    void showNodeInfo() {
        // Demonstrate in-process RPC calls
        try {
            if (m_node && m_node->isRunning()) {
                // Get blockchain info
                Json::Json::Value params(Json::Json::Value(Json::arrayJson::Value));
                Json::Json::Value result = m_node->executeRpcJson("getblockchaininfo", params);
                
                if (!result.contains("error")) {
                    int blocks = result.value("blocks", 0);
                    std::string chain = result.value("chain", "unknown");
                    
                    QString info = QString("Chain: %1, Blocks: %2")
                                 .arg(QString::fromStdString(chain))
                                 .arg(blocks);
                    
                    m_status_label->setText("Node: Running ✓ - " + info);
                }
            }
        } catch (const std::exception& e) {
            // Ignore errors in demo
        }
    }
    
private:
    std::unique_ptr<dinero::interfaces::Node> m_node;
    InitExecutor* m_init_executor = nullptr;
    QThread* m_init_thread = nullptr;
    
    QLabel* m_status_label;
    RpcConsole* m_rpc_console;
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    
    // Set application properties
    app.setApplicationName("Dinero Bitcoin-Qt Style Demo");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("Dinero Project");
    
    MainWindow window;
    window.show();
    
    return app.exec();
}

#include "qt-rpc-console-example.moc"
