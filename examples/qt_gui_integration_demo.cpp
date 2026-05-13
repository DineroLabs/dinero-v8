// Qt6 Desktop GUI - RPC Integration Demo
// Shows how every RPC endpoint maps to GUI features

#include <QApplication>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QEventLoop>
#include <QFile>
#include <QDateTime>
#include <QGroupBox>
#include <QListWidget>
#include <QTextEdit>
#include <QTabWidget>

class DineroDashboard : public QMainWindow {
    Q_OBJECT

public:
    DineroDashboard(QWidget* parent = nullptr) : QMainWindow(parent) {
        setupUI();
        setupTimer();
        refreshAll();
    }

private slots:
    void refreshAll() {
        refreshDashboard();
        refreshBlockList();
        refreshMempool();
        refreshHealth();
    }

    void showBlockDetail(const QString& hash) {
        auto block = rpcCall("blockchain.getblock", QJsonArray{hash, 1}).value("result").toObject();
        
        QString detail = QString(
            "Block: %1\n"
            "Height: %2\n"
            "Version: %3\n"
            "Time: %4\n"
            "Bits: 0x%5\n"
            "Nonce: %6\n"
            "Previous: %7\n"
            "Merkle: %8\n"
            "Chainwork: %9\n"
            "Transactions: %10"
        ).arg(hash)
         .arg(block["height"].toInt())
         .arg(block["version"].toInt())
         .arg(QDateTime::fromSecsSinceEpoch(block["time"].toInt()).toString())
         .arg(block["bits"].toInt(), 8, 16, QChar('0'))
         .arg(block["nonce"].toInt())
         .arg(block["previousblockhash"].toString())
         .arg(block["merkleroot"].toString())
         .arg(block["chainwork"].toString())
         .arg(block["nTx"].toInt());
         
        blockDetailText->setText(detail);
    }

private:
    void setupUI() {
        auto central = new QWidget(this);
        setCentralWidget(central);
        
        auto layout = new QVBoxLayout(central);
        
        // Create tabs
        auto tabs = new QTabWidget;
        tabs->addTab(createDashboardTab(), "Dashboard");
        tabs->addTab(createBlocksTab(), "Blocks");
        tabs->addTab(createMempoolTab(), "Mempool");
        tabs->addTab(createHealthTab(), "Health");
        
        layout->addWidget(tabs);
        
        setWindowTitle("Dinero Desktop - Production Ready");
        resize(800, 600);
    }
    
    QWidget* createDashboardTab() {
        auto widget = new QWidget;
        auto layout = new QVBoxLayout(widget);
        
        // Network status group
        auto networkGroup = new QGroupBox("Network Status");
        auto networkLayout = new QHBoxLayout(networkGroup);
        
        networkBadge = new QLabel("REGTEST");
        networkBadge->setStyleSheet("background: #28a745; color: white; padding: 4px 8px; border-radius: 4px; font-weight: bold;");
        
        heightLabel = new QLabel("Height: 0");
        tipLabel = new QLabel("Tip: genesis...");
        uptimeLabel = new QLabel("Uptime: 0s");
        
        networkLayout->addWidget(networkBadge);
        networkLayout->addWidget(heightLabel);
        networkLayout->addWidget(tipLabel);
        networkLayout->addWidget(uptimeLabel);
        networkLayout->addStretch();
        
        // Blockchain info group
        auto infoGroup = new QGroupBox("Blockchain Info");
        auto infoLayout = new QVBoxLayout(infoGroup);
        
        chainworkLabel = new QLabel("Chainwork: 0000...0001");
        difficultyLabel = new QLabel("Difficulty: 1");
        blocksLabel = new QLabel("Blocks: 0");
        
        infoLayout->addWidget(chainworkLabel);
        infoLayout->addWidget(difficultyLabel);
        infoLayout->addWidget(blocksLabel);
        
        layout->addWidget(networkGroup);
        layout->addWidget(infoGroup);
        layout->addStretch();
        
        return widget;
    }
    
    QWidget* createBlocksTab() {
        auto widget = new QWidget;
        auto layout = new QVBoxLayout(widget);
        
        blockList = new QListWidget;
        connect(blockList, &QListWidget::itemClicked, [this](QListWidgetItem* item) {
            auto hash = item->data(Qt::UserRole).toString();
            showBlockDetail(hash);
        });
        
        blockDetailText = new QTextEdit;
        blockDetailText->setMaximumHeight(200);
        
        layout->addWidget(new QLabel("Recent Blocks:"));
        layout->addWidget(blockList);
        layout->addWidget(new QLabel("Block Detail:"));
        layout->addWidget(blockDetailText);
        
        return widget;
    }
    
    QWidget* createMempoolTab() {
        auto widget = new QWidget;
        auto layout = new QVBoxLayout(widget);
        
        mempoolSizeLabel = new QLabel("Size: 0 transactions");
        mempoolBytesLabel = new QLabel("Bytes: 0");
        mempoolUsageLabel = new QLabel("Memory: 0");
        
        layout->addWidget(new QLabel("Mempool Status:"));
        layout->addWidget(mempoolSizeLabel);
        layout->addWidget(mempoolBytesLabel);
        layout->addWidget(mempoolUsageLabel);
        layout->addStretch();
        
        return widget;
    }
    
    QWidget* createHealthTab() {
        auto widget = new QWidget;
        auto layout = new QVBoxLayout(widget);
        
        healthStatusLabel = new QLabel("Status: Checking...");
        healthNetworkLabel = new QLabel("Network: Unknown");
        healthHeightLabel = new QLabel("Height: Unknown");
        
        metricsText = new QTextEdit;
        metricsText->setMaximumHeight(300);
        
        layout->addWidget(new QLabel("Daemon Health:"));
        layout->addWidget(healthStatusLabel);
        layout->addWidget(healthNetworkLabel);
        layout->addWidget(healthHeightLabel);
        layout->addWidget(new QLabel("Metrics:"));
        layout->addWidget(metricsText);
        
        return widget;
    }
    
    void setupTimer() {
        refreshTimer = new QTimer(this);
        connect(refreshTimer, &QTimer::timeout, this, &DineroDashboard::refreshAll);
        refreshTimer->start(5000); // 5 second refresh
    }
    
    void refreshDashboard() {
        try {
            auto info = rpcCall("blockchain.getinfo").value("result").toObject();
            auto uptime = rpcCall("uptime").value("result").toInt();
            
            // Update network badge
            auto chain = info["chain"].toString().toUpper();
            networkBadge->setText(chain);
            networkBadge->setStyleSheet(QString(
                "background: %1; color: white; padding: 4px 8px; border-radius: 4px; font-weight: bold;"
            ).arg(chain == "REGTEST" ? "#ffc107" : chain == "TESTNET" ? "#17a2b8" : "#28a745"));
            
            // Update labels
            heightLabel->setText(QString("Height: %1").arg(info["blocks"].toInt()));
            tipLabel->setText(QString("Tip: %1...").arg(info["bestblockhash"].toString().left(8)));
            chainworkLabel->setText(QString("Chainwork: %1...").arg(info["chainwork"].toString().left(8)));
            difficultyLabel->setText(QString("Difficulty: %1").arg(info["difficulty_str"].toString()));
            blocksLabel->setText(QString("Blocks: %1").arg(info["blocks"].toInt()));
            uptimeLabel->setText(QString("Uptime: %1").arg(formatDuration(uptime)));
            
        } catch (...) {
            networkBadge->setText("OFFLINE");
            networkBadge->setStyleSheet("background: #dc3545; color: white; padding: 4px 8px; border-radius: 4px; font-weight: bold;");
        }
    }
    
    void refreshBlockList() {
        try {
            auto height = rpcCall("blockchain.getblockcount").value("result").toInt();
            
            blockList->clear();
            for (int h = height; h >= 0 && h > height - 10; h--) {
                auto hash = rpcCall("blockchain.getblockhash", QJsonArray{h}).value("result").toString();
                auto header = rpcCall("blockchain.getblockheader", QJsonArray{hash, true}).value("result").toObject();
                
                auto item = new QListWidgetItem(QString("Height %1: %2... (%3 tx)")
                    .arg(h)
                    .arg(hash.left(8))
                    .arg(header["nTx"].toInt()));
                item->setData(Qt::UserRole, hash);
                blockList->addItem(item);
            }
        } catch (...) {
            // Handle error gracefully
        }
    }
    
    void refreshMempool() {
        try {
            auto mempool = rpcCall("mempool.getinfo").value("result").toObject();
            
            mempoolSizeLabel->setText(QString("Size: %1 transactions").arg(mempool["size"].toInt()));
            mempoolBytesLabel->setText(QString("Bytes: %1").arg(formatBytes(mempool["bytes"].toInt())));
            mempoolUsageLabel->setText(QString("Memory: %1").arg(formatBytes(mempool["usage"].toInt())));
            
        } catch (...) {
            mempoolSizeLabel->setText("Size: Unknown");
            mempoolBytesLabel->setText("Bytes: Unknown");
            mempoolUsageLabel->setText("Memory: Unknown");
        }
    }
    
    void refreshHealth() {
        try {
            // Try /healthz endpoint
            auto health = httpGet("http://127.0.0.1:20998/healthz");
            
            healthStatusLabel->setText(QString("Status: %1")
                .arg(health["status"].toString() == "ok" ? "Ready" : "Degraded"));
            healthNetworkLabel->setText(QString("Network: %1").arg(health["network"].toString()));
            healthHeightLabel->setText(QString("Height: %1").arg(health["height"].toInt()));
            
            // Try /metrics endpoint
            auto metrics = httpGetText("http://127.0.0.1:20998/metrics");
            metricsText->setText(metrics.left(1000) + (metrics.length() > 1000 ? "\n..." : ""));
            
        } catch (...) {
            healthStatusLabel->setText("Status: Unavailable");
            healthNetworkLabel->setText("Network: Unknown");
            healthHeightLabel->setText("Height: Unknown");
            metricsText->setText("Metrics unavailable");
        }
    }
    
    // RPC helper
    QJsonObject rpcCall(const QString& method, const QJsonArray& params = QJsonArray()) {
        QNetworkRequest r(QUrl("http://127.0.0.1:20998/"));
        r.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        
        // Load cookie for authentication
        QFile cookie("data/regtest/.cookie"); 
        if (cookie.open(QIODevice::ReadOnly)) {
            r.setRawHeader("Authorization", "Basic " + cookie.readAll().toBase64());
        }
        
        // Build JSON-RPC request
        QJsonObject body{
            {"jsonrpc", "2.0"},
            {"id", "gui"},
            {"method", method},
            {"params", params}
        };
        
        // Synchronous request
        QNetworkAccessManager nm;
        QEventLoop loop;
        auto reply = nm.post(r, QJsonDocument(body).toJson());
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        
        auto response = QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        return response;
    }
    
    // HTTP GET helpers
    QJsonObject httpGet(const QString& url) {
        QNetworkAccessManager nm;
        QEventLoop loop;
        auto reply = nm.get(QNetworkRequest(QUrl(url)));
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        
        auto response = QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        return response;
    }
    
    QString httpGetText(const QString& url) {
        QNetworkAccessManager nm;
        QEventLoop loop;
        auto reply = nm.get(QNetworkRequest(QUrl(url)));
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        
        auto response = reply->readAll();
        reply->deleteLater();
        return response;
    }
    
    // Utility functions
    QString formatDuration(int seconds) {
        if (seconds < 60) return QString("%1s").arg(seconds);
        if (seconds < 3600) return QString("%1m %2s").arg(seconds/60).arg(seconds%60);
        return QString("%1h %2m").arg(seconds/3600).arg((seconds%3600)/60);
    }
    
    QString formatBytes(qint64 bytes) {
        if (bytes < 1024) return QString("%1 B").arg(bytes);
        if (bytes < 1024*1024) return QString("%1 KB").arg(bytes/1024.0, 0, 'f', 1);
        return QString("%1 MB").arg(bytes/(1024.0*1024.0), 0, 'f', 1);
    }
    
    // UI elements
    QLabel* networkBadge;
    QLabel* heightLabel;
    QLabel* tipLabel;
    QLabel* uptimeLabel;
    QLabel* chainworkLabel;
    QLabel* difficultyLabel;
    QLabel* blocksLabel;
    
    QListWidget* blockList;
    QTextEdit* blockDetailText;
    
    QLabel* mempoolSizeLabel;
    QLabel* mempoolBytesLabel;
    QLabel* mempoolUsageLabel;
    
    QLabel* healthStatusLabel;
    QLabel* healthNetworkLabel;
    QLabel* healthHeightLabel;
    QTextEdit* metricsText;
    
    QTimer* refreshTimer;
};

// Demo application
int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    
    DineroDashboard dashboard;
    dashboard.show();
    
    return app.exec();
}

#include "qt_gui_integration_demo.moc"
