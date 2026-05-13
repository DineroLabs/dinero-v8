#pragma once

#include <QDialog>

class QTextEdit;
class RpcClient;

/**
 * About Dialog with comprehensive version information
 * Shows app version, git SHA, build date, Qt version, system info, and daemon details
 */
class AboutDialog : public QDialog {
    Q_OBJECT

public:
    explicit AboutDialog(QWidget* parent = nullptr);
    
    void setRpcClient(RpcClient* rpcClient);

private slots:
    void copyVersionInfo();

private:
    void setupUI();
    void loadVersionInfo();
    void loadDaemonVersionInfo();
    
    QTextEdit* m_versionInfo = nullptr;
    RpcClient* m_rpcClient = nullptr;
};
