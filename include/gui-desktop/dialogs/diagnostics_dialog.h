#pragma once

#include <QDialog>
#include <QJsonObject>

class QTextEdit;
class QPushButton;
class QProgressBar;
class RpcClient;

/**
 * Diagnostics Dialog
 * Collects system info, daemon status, and exports support bundle
 */
class DiagnosticsDialog : public QDialog {
    Q_OBJECT

public:
    explicit DiagnosticsDialog(QWidget* parent = nullptr);
    
    void setRpcClient(RpcClient* rpcClient);

private slots:
    void collectDiagnostics();
    void exportBundle();

private:
    void setupUI();
    void collectSystemInfo();
    void collectApplicationInfo();
    void collectDaemonInfo();
    void finishRpcCall();
    void finishCollection();
    void appendLog(const QString& text);
    
    RpcClient* m_rpcClient = nullptr;
    QTextEdit* m_diagnosticsOutput = nullptr;
    QPushButton* m_collectBtn = nullptr;
    QPushButton* m_exportBtn = nullptr;
    QProgressBar* m_progressBar = nullptr;
    
    QJsonObject m_diagnosticsData;
    int m_pendingRpcCalls = 0;
};
