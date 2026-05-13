#pragma once

#include <QWizard>
#include <QWizardPage>
#include <QLineEdit>
#include <QLabel>
#include <QTextEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QButtonGroup>
#include <QTimer>

class RpcClient;
class ConnectionManager;

// ============================================================================
// Page 1: Welcome - Choose Create, Restore, or Import Taproot
// ============================================================================
class WelcomePage : public QWizardPage {
  Q_OBJECT
public:
  explicit WelcomePage(RpcClient* rpc, QWidget* parent = nullptr);
  void initializePage() override;
  bool validatePage() override;
  int nextId() const override;

private:
  void updateWalletNameUi();
  RpcClient* rpc_;
  QButtonGroup* choiceGroup_;
  QLineEdit* edtWalletName_;
  QLabel* lblWalletNameHint_;
  QLabel* lblUnloadNotice_;
  static constexpr int PAGE_CREATE_SEED = 1;
  static constexpr int PAGE_RESTORE_SEED = 2;
  static constexpr int PAGE_IMPORT_TAPROOT = 6;  // New: Import Taproot descriptor
};

// ============================================================================
// Page 2: Create New Wallet - Display Seed
// ============================================================================
class CreateSeedPage : public QWizardPage {
  Q_OBJECT
public:
  explicit CreateSeedPage(ConnectionManager* connMgr, QWidget* parent = nullptr);
  void initializePage() override;
  bool validatePage() override;
  int nextId() const override;
  
  QString getSeedPhrase() const { return seedPhrase_; }
  
private Q_SLOTS:
  void onGenerateSeed();
  void onRevealClicked();
  
private:
  QString formatSeedPhrase(const QString& seed) const;
  void setSeedVisible(bool visible);
  ConnectionManager* connMgr_;
  QString seedPhrase_;
  QLabel* lblSeed_;
  QPushButton* btnReveal_;
  QCheckBox* chkCopied_;
  QTimer* rpcTimeout_;
  bool seedGenerated_;
  bool seedVisible_;
};

// ============================================================================
// Page 3: Confirm Seed (3 random words)
// ============================================================================
class ConfirmSeedPage : public QWizardPage {
  Q_OBJECT
public:
  explicit ConfirmSeedPage(QWidget* parent = nullptr);
  void initializePage() override;
  bool validatePage() override;
  int nextId() const override;
  
private:
  QLineEdit* edtWord1_;
  QLineEdit* edtWord2_;
  QLineEdit* edtWord3_;
  int index1_, index2_, index3_;
  QLabel* lblPrompt_;
};

// ============================================================================
// Page 4: Restore Wallet - Enter Seed
// ============================================================================
class RestoreSeedPage : public QWizardPage {
  Q_OBJECT
public:
  explicit RestoreSeedPage(QWidget* parent = nullptr);
  bool validatePage() override;
  int nextId() const override;
  
  QString getSeedPhrase() const;
  QString getPassphrase() const { return edtPassphrase_->text(); }
  
private Q_SLOTS:
  void onSeedChanged();
  
private:
  QTextEdit* txtSeed_;
  QLineEdit* edtPassphrase_;
  QCheckBox* chkSkipChecksum_;
  QLabel* lblStatus_;
  bool validateBIP39Seed(const QString& seed);
};

// ============================================================================
// Page 5: Set Encryption Password
// ============================================================================
class SetPasswordPage : public QWizardPage {
  Q_OBJECT
public:
  explicit SetPasswordPage(QWidget* parent = nullptr);
  bool validatePage() override;
  int nextId() const override;
  
  QString getPassword() const { return edtPassword1_->text(); }
  
private Q_SLOTS:
  void onPasswordChanged();
  
private:
  QLineEdit* edtPassword1_;
  QLineEdit* edtPassword2_;
  QLabel* lblStrength_;
  QLabel* lblMatch_;
  
  QString calculatePasswordStrength(const QString& password);
};

// ============================================================================
// Page 6: Success / Completion
// ============================================================================
class CompletionPage : public QWizardPage {
  Q_OBJECT
public:
  explicit CompletionPage(RpcClient* rpc, QWidget* parent = nullptr);
  void initializePage() override;
  int nextId() const override { return -1; } // Final page

private:
  RpcClient* rpc_;
  QLabel* lblWalletName_;
  QLabel* lblFingerprint_;
  QLabel* lblFirstAddress_;
  QLabel* lblStatus_;
};

// ============================================================================
// Page 7: Import Taproot Descriptor
// ============================================================================
class ImportTaprootPage : public QWizardPage {
  Q_OBJECT
public:
  explicit ImportTaprootPage(ConnectionManager* connMgr, QWidget* parent = nullptr);
  void initializePage() override;
  bool validatePage() override;
  int nextId() const override;

  QString getDescriptor() const;
  QString getLabel() const { return edtLabel_->text().trimmed(); }

private Q_SLOTS:
  void onDescriptorChanged();
  void onImportClicked();

private:
  ConnectionManager* connMgr_;
  QLineEdit* edtDescriptor_;
  QLineEdit* edtLabel_;
  QLabel* lblStatus_;
  QLabel* lblAddress_;
  QPushButton* btnImport_;
  bool importSucceeded_;
  QString importedAddress_;
};

// ============================================================================
// Main Wallet Wizard
// ============================================================================
class WalletWizard : public QWizard {
  Q_OBJECT
public:
  explicit WalletWizard(ConnectionManager* connMgr, RpcClient* rpc, QWidget* parent = nullptr);
  
  enum PageId {
    Page_Welcome = 0,
    Page_CreateSeed = 1,
    Page_RestoreSeed = 2,
    Page_ConfirmSeed = 3,
    Page_SetPassword = 4,
    Page_Completion = 5,
    Page_ImportTaproot = 6
  };
  
  // Access to connection manager and RPC client for pages
  ConnectionManager* connectionManager() const { return connMgr_; }
  RpcClient* rpcClient() const { return rpc_; }
  
Q_SIGNALS:
  void walletCreated(const QString& walletName, const QString& fingerprint, bool restored);
  
private Q_SLOTS:
  void onFinished(int result);
  
private:
  ConnectionManager* connMgr_;
  RpcClient* rpc_;
};
