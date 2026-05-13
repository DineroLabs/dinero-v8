#include "walletwizard.h"
#include "rpcclient.h"
#include "connection_manager.h"
#include "walletnameutils.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QRadioButton>
#include <QGroupBox>
#include <QMessageBox>
#include <QClipboard>
#include <QApplication>
#include <QRandomGenerator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QDebug>
#include <QTimer>
#include <QEventLoop>
#include <memory>

namespace {

QStringList extractWalletNames(const QJsonValue& result) {
  QStringList walletNames;
  QJsonArray wallets;

  if (result.isArray()) {
    wallets = result.toArray();
  } else if (result.isObject()) {
    wallets = result.toObject().value("wallets").toArray();
  }

  for (const auto& walletVal : wallets) {
    QString walletName;
    if (walletVal.isObject()) {
      walletName = walletVal.toObject().value("name").toString().trimmed();
    } else if (walletVal.isString()) {
      walletName = walletVal.toString().trimmed();
    }

    if (!walletName.isEmpty() && !walletNames.contains(walletName)) {
      walletNames.append(walletName);
    }
  }

  return walletNames;
}

bool fetchExistingWalletNames(RpcClient* rpc, QStringList* walletNames, QString* errorMessage) {
  if (walletNames) {
    walletNames->clear();
  }
  if (errorMessage) {
    errorMessage->clear();
  }

  if (!rpc) {
    if (errorMessage) {
      *errorMessage = QStringLiteral("Wallet RPC is not available.");
    }
    return false;
  }

  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);

  bool finished = false;
  bool success = false;
  QStringList discoveredWallets;
  QString failureReason;

  QMetaObject::Connection resultConn;
  QMetaObject::Connection errorConn;
  QMetaObject::Connection timeoutConn;

  resultConn = QObject::connect(rpc, &RpcClient::rpcResult, &loop,
    [&](const QString& method, const QJsonValue& result) {
      if (method != "wallet.listwallets") {
        return;
      }
      discoveredWallets = extractWalletNames(result);
      finished = true;
      success = true;
      loop.quit();
    });

  errorConn = QObject::connect(rpc, &RpcClient::rpcError, &loop,
    [&](const QString& method, int code, const QString& message) {
      Q_UNUSED(code);
      if (method != "wallet.listwallets") {
        return;
      }
      finished = true;
      failureReason = message.trimmed();
      loop.quit();
    });

  timeoutConn = QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
    if (finished) {
      return;
    }
    finished = true;
    failureReason = QStringLiteral("Timed out while querying existing wallets.");
    loop.quit();
  });

  timeout.start(15000);
  rpc->call("wallet.listwallets", QJsonArray());
  loop.exec();

  QObject::disconnect(resultConn);
  QObject::disconnect(errorConn);
  QObject::disconnect(timeoutConn);

  if (success && walletNames) {
    *walletNames = discoveredWallets;
  }
  if (!success && errorMessage) {
    *errorMessage = failureReason.isEmpty()
      ? QStringLiteral("Failed to query existing wallets.")
      : failureReason;
  }
  return success;
}

QString currentWizardWalletName(QWizard* wizard) {
  return wizard ? wizard->field("walletName").toString().trimmed() : QString();
}

bool unloadWalletForProvisioning(RpcClient* rpc, const QString& walletName, QString* errorMessage) {
  if (errorMessage) {
    errorMessage->clear();
  }

  if (!rpc) {
    if (errorMessage) {
      *errorMessage = QStringLiteral("Wallet RPC is not available.");
    }
    return false;
  }

  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);

  bool success = false;
  QString failureReason;

  QMetaObject::Connection resultConn;
  QMetaObject::Connection errorConn;
  QMetaObject::Connection timeoutConn;

  resultConn = QObject::connect(rpc, &RpcClient::rpcResult, &loop,
    [&](const QString& method, const QJsonValue& result) {
      if (method != "wallet.unload") {
        return;
      }

      if (result.isObject()) {
        const auto obj = result.toObject();
        success = obj.value("success").toBool(false);
        if (!success) {
          failureReason = obj.value("error").toString(QStringLiteral("Could not unload the active wallet."));
        }
      } else {
        failureReason = QStringLiteral("Invalid wallet.unload response.");
      }
      loop.quit();
    });

  errorConn = QObject::connect(rpc, &RpcClient::rpcError, &loop,
    [&](const QString& method, int code, const QString& message) {
      Q_UNUSED(code);
      if (method != "wallet.unload") {
        return;
      }
      failureReason = message.trimmed();
      loop.quit();
    });

  timeoutConn = QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
    failureReason = QStringLiteral("Timed out while unloading the active wallet.");
    loop.quit();
  });

  timeout.start(15000);
  rpc->callNamed("wallet.unload", QJsonObject{{"name", walletName}});
  loop.exec();

  QObject::disconnect(resultConn);
  QObject::disconnect(errorConn);
  QObject::disconnect(timeoutConn);

  if (!success && errorMessage) {
    *errorMessage = failureReason.isEmpty()
      ? QStringLiteral("Could not unload the active wallet.")
      : failureReason;
  }

  return success;
}

bool callNamedRpcObject(RpcClient* rpc,
                        const QString& method,
                        const QJsonObject& params,
                        QJsonObject* resultObject,
                        QString* errorMessage) {
  if (resultObject) {
    *resultObject = QJsonObject();
  }
  if (errorMessage) {
    errorMessage->clear();
  }

  if (!rpc) {
    if (errorMessage) {
      *errorMessage = QStringLiteral("Wallet RPC is not available.");
    }
    return false;
  }

  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);

  bool success = false;
  QString failureReason;
  QJsonObject replyObject;

  QMetaObject::Connection resultConn;
  QMetaObject::Connection errorConn;
  QMetaObject::Connection timeoutConn;

  resultConn = QObject::connect(rpc, &RpcClient::rpcResult, &loop,
    [&](const QString& replyMethod, const QJsonValue& result) {
      if (replyMethod != method) {
        return;
      }
      if (result.isObject()) {
        replyObject = result.toObject();
        success = !replyObject.contains("success") || replyObject.value("success").toBool(false);
        if (!success) {
          failureReason = replyObject.value("error").toString(QStringLiteral("RPC call failed."));
        }
      } else {
        failureReason = QStringLiteral("Invalid RPC response.");
      }
      loop.quit();
    });

  errorConn = QObject::connect(rpc, &RpcClient::rpcError, &loop,
    [&](const QString& replyMethod, int code, const QString& message) {
      Q_UNUSED(code);
      if (replyMethod != method) {
        return;
      }
      failureReason = message.trimmed();
      loop.quit();
    });

  timeoutConn = QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
    failureReason = QStringLiteral("Timed out while calling %1.").arg(method);
    loop.quit();
  });

  timeout.start(5000);
  rpc->callNamed(method, params);
  loop.exec();

  QObject::disconnect(resultConn);
  QObject::disconnect(errorConn);
  QObject::disconnect(timeoutConn);

  if (resultObject) {
    *resultObject = replyObject;
  }
  if (!success && errorMessage) {
    *errorMessage = failureReason.isEmpty() ? QStringLiteral("RPC call failed.") : failureReason;
  }
  return success;
}

bool callArrayRpcObject(RpcClient* rpc,
                        const QString& method,
                        const QJsonArray& params,
                        QJsonObject* resultObject,
                        QString* errorMessage) {
  if (resultObject) {
    *resultObject = QJsonObject();
  }
  if (errorMessage) {
    errorMessage->clear();
  }

  if (!rpc) {
    if (errorMessage) {
      *errorMessage = QStringLiteral("Wallet RPC is not available.");
    }
    return false;
  }

  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);

  bool success = false;
  QString failureReason;
  QJsonObject replyObject;

  QMetaObject::Connection resultConn;
  QMetaObject::Connection errorConn;
  QMetaObject::Connection timeoutConn;

  resultConn = QObject::connect(rpc, &RpcClient::rpcResult, &loop,
    [&](const QString& replyMethod, const QJsonValue& result) {
      if (replyMethod != method) {
        return;
      }
      if (result.isObject()) {
        replyObject = result.toObject();
        success = !replyObject.contains("success") || replyObject.value("success").toBool(false);
        if (!success) {
          failureReason = replyObject.value("error").toString(QStringLiteral("RPC call failed."));
        }
      } else {
        failureReason = QStringLiteral("Invalid RPC response.");
      }
      loop.quit();
    });

  errorConn = QObject::connect(rpc, &RpcClient::rpcError, &loop,
    [&](const QString& replyMethod, int code, const QString& message) {
      Q_UNUSED(code);
      if (replyMethod != method) {
        return;
      }
      failureReason = message.trimmed();
      loop.quit();
    });

  timeoutConn = QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
    failureReason = QStringLiteral("Timed out while calling %1.").arg(method);
    loop.quit();
  });

  timeout.start(5000);
  rpc->call(method, params);
  loop.exec();

  QObject::disconnect(resultConn);
  QObject::disconnect(errorConn);
  QObject::disconnect(timeoutConn);

  if (resultObject) {
    *resultObject = replyObject;
  }
  if (!success && errorMessage) {
    *errorMessage = failureReason.isEmpty() ? QStringLiteral("RPC call failed.") : failureReason;
  }
  return success;
}

bool deleteWalletForProvisioning(RpcClient* rpc, const QString& walletName, QString* errorMessage) {
  return callNamedRpcObject(rpc, "wallet.delete", QJsonObject{{"name", walletName}}, nullptr, errorMessage);
}

bool openWalletAfterProvisioning(RpcClient* rpc, const QString& walletName, QString* errorMessage) {
  return callArrayRpcObject(rpc, "wallet.open", QJsonArray{walletName}, nullptr, errorMessage);
}

bool rollbackProvisionedWallet(QWizard* wizard, QString* errorMessage) {
  if (errorMessage) {
    errorMessage->clear();
  }

  auto* walletWizard = qobject_cast<WalletWizard*>(wizard);
  if (!walletWizard || !walletWizard->rpcClient()) {
    return false;
  }

  const QString walletName = [&]() {
    const QString provisioned = wizard->property("provisionedWalletName").toString().trimmed();
    if (!provisioned.isEmpty()) {
      return provisioned;
    }
    return wizard->property("walletRollbackCandidateName").toString().trimmed();
  }();

  const QString previousWallet = wizard->property("setupPreviousWalletName").toString().trimmed();
  const bool walletWasUnloaded = wizard->property("walletWasUnloadedForSetup").toBool();

  QStringList failures;
  if (!walletName.isEmpty()) {
    QString deleteError;
    if (!deleteWalletForProvisioning(walletWizard->rpcClient(), walletName, &deleteError)) {
      failures << QString("Could not delete provisional wallet '%1': %2").arg(walletName, deleteError);
    }
  }

  if (walletWasUnloaded && !previousWallet.isEmpty()) {
    QString openError;
    if (!openWalletAfterProvisioning(walletWizard->rpcClient(), previousWallet, &openError)) {
      failures << QString("Could not reopen previous wallet '%1': %2").arg(previousWallet, openError);
    }
  }

  wizard->setProperty("provisionedWalletName", QString());
  wizard->setProperty("provisionedMode", QString());
  wizard->setProperty("walletRollbackCandidateName", QString());
  wizard->setProperty("walletWasUnloadedForSetup", false);

  if (errorMessage && !failures.isEmpty()) {
    *errorMessage = failures.join("\n");
  }
  return failures.isEmpty();
}

} // namespace

// ============================================================================
// Page 1: Welcome
// ============================================================================
WelcomePage::WelcomePage(RpcClient* rpc, QWidget* parent)
    : QWizardPage(parent)
    , rpc_(rpc)
    , edtWalletName_(nullptr)
    , lblWalletNameHint_(nullptr)
    , lblUnloadNotice_(nullptr) {
  setTitle("Welcome to Dinero Wallet");
  setSubTitle("Create a new wallet or restore an existing one");

  auto* layout = new QVBoxLayout;

  auto* intro = new QLabel(
    "<p>Your Dinero wallet will be protected with:</p>"
    "<ul>"
    "<li><b>12-word BIP-39 seed phrase</b> (industry standard, 128-bit security)</li>"
    "<li><b>AES-256-GCM encryption</b> with Argon2id key derivation</li>"
    "<li><b>HD wallet</b> (BIP-32/86 Taproot) for unlimited addresses</li>"
    "</ul>"
  );
  intro->setWordWrap(true);
  layout->addWidget(intro);

  auto* choiceGroup = new QGroupBox("Choose an option:");
  auto* choiceLayout = new QVBoxLayout(choiceGroup);

  choiceGroup_ = new QButtonGroup(this);
  auto* radioCreate = new QRadioButton("🆕 Create a new wallet");
  auto* radioRestore = new QRadioButton("♻️ Emergency restore from seed phrase");
  auto* radioImportTaproot = new QRadioButton("🔑 Import Taproot descriptor (advanced)");

  choiceGroup_->addButton(radioCreate, PAGE_CREATE_SEED);
  choiceGroup_->addButton(radioRestore, PAGE_RESTORE_SEED);
  choiceGroup_->addButton(radioImportTaproot, PAGE_IMPORT_TAPROOT);
  radioCreate->setChecked(true);

  choiceLayout->addWidget(radioCreate);
  choiceLayout->addWidget(radioRestore);
  choiceLayout->addWidget(radioImportTaproot);
  layout->addWidget(choiceGroup);

  auto* walletNameGroup = new QGroupBox("Wallet name");
  auto* walletNameLayout = new QVBoxLayout(walletNameGroup);

  edtWalletName_ = new QLineEdit;
  edtWalletName_->setPlaceholderText("default");
  edtWalletName_->setText("default");
  edtWalletName_->setClearButtonEnabled(true);
  walletNameLayout->addWidget(edtWalletName_);
  registerField("walletName", edtWalletName_);

  lblWalletNameHint_ = new QLabel(
    "Balances, addresses, send history, and advisory state stay scoped to this wallet only. "
    "Existing wallet names cannot be overwritten from the setup wizard."
  );
  lblWalletNameHint_->setWordWrap(true);
  lblWalletNameHint_->setStyleSheet("QLabel { color: #adb5bd; font-size: 11px; }");
  walletNameLayout->addWidget(lblWalletNameHint_);

  lblUnloadNotice_ = new QLabel;
  lblUnloadNotice_->setWordWrap(true);
  lblUnloadNotice_->setVisible(false);
  lblUnloadNotice_->setStyleSheet(
    "QLabel {"
    "  color: #ffd166;"
    "  background-color: rgba(255, 209, 102, 0.08);"
    "  border: 1px solid rgba(255, 209, 102, 0.28);"
    "  border-radius: 6px;"
    "  padding: 8px;"
    "  font-size: 11px;"
    "}"
  );
  walletNameLayout->addWidget(lblUnloadNotice_);
  layout->addWidget(walletNameGroup);

  connect(edtWalletName_, &QLineEdit::editingFinished, this, [this]() {
    const QString normalized = normalizeWalletName(edtWalletName_->text());
    if (!normalized.isEmpty()) {
      edtWalletName_->setText(normalized);
    }
  });

  auto updateChoiceUi = [this]() {
    updateWalletNameUi();
  };
  connect(choiceGroup_, &QButtonGroup::idClicked, this, updateChoiceUi);
  connect(choiceGroup_, &QButtonGroup::idToggled, this,
          [this](int, bool) { updateWalletNameUi(); });

  // Note about Taproot import
  auto* taprootNote = new QLabel(
    "<p style='font-size: 10px; color: #666;'>"
    "<b>Note:</b> Taproot descriptor import is for advanced users who have a "
    "tr(&lt;privkey&gt;) descriptor from mining or another wallet. For most users, "
    "creating a new wallet or restoring from seed phrase is recommended.</p>"
  );
  taprootNote->setWordWrap(true);
  layout->addWidget(taprootNote);

  layout->addStretch();
  setLayout(layout);
  updateWalletNameUi();
}

void WelcomePage::initializePage() {
  updateWalletNameUi();
}

void WelcomePage::updateWalletNameUi() {
  const bool importSelected = choiceGroup_ && choiceGroup_->checkedId() == PAGE_IMPORT_TAPROOT;
  const bool provisioningLocked = wizard() && !wizard()->property("provisionedWalletName").toString().isEmpty();
  const QString activeWalletBeforeSetup =
      wizard() ? wizard()->property("setupPreviousWalletName").toString().trimmed() : QString();

  if (choiceGroup_) {
    const auto buttons = choiceGroup_->buttons();
    for (auto* button : buttons) {
      if (button) {
        button->setEnabled(!provisioningLocked);
      }
    }
  }

  if (edtWalletName_) {
    edtWalletName_->setVisible(!importSelected);
    edtWalletName_->setEnabled(!provisioningLocked && !importSelected);
  }

  if (lblWalletNameHint_) {
    if (importSelected) {
      lblWalletNameHint_->setText(
        "Taproot descriptor import uses the currently active wallet. Load a wallet first if needed."
      );
    } else if (provisioningLocked) {
      const QString walletName = wizard()->property("provisionedWalletName").toString();
      lblWalletNameHint_->setText(
        QString("Wallet '%1' is already provisioned in this setup session. Finish or cancel this wizard to change it.")
          .arg(walletName)
      );
    } else {
      lblWalletNameHint_->setText(
        "Balances, addresses, send history, and advisory state stay scoped to this wallet only. "
        "Existing wallet names cannot be overwritten from the setup wizard."
      );
    }
  }

  if (lblUnloadNotice_) {
    if (!importSelected && !provisioningLocked && !activeWalletBeforeSetup.isEmpty()) {
      lblUnloadNotice_->setText(
        QString("Current wallet '%1' is still loaded. When you click Next, Dinero will unload it "
                "before create/restore continues so balances, addresses, and pending wallet state cannot mix.")
            .arg(activeWalletBeforeSetup)
      );
      lblUnloadNotice_->setVisible(true);
    } else if (importSelected && !activeWalletBeforeSetup.isEmpty()) {
      lblUnloadNotice_->setText(
        QString("Current wallet '%1' stays loaded for Taproot descriptor import. "
                "Dinero will not unload it for this mode.")
            .arg(activeWalletBeforeSetup)
      );
      lblUnloadNotice_->setVisible(true);
    } else {
      lblUnloadNotice_->clear();
      lblUnloadNotice_->setVisible(false);
    }
  }
}

bool WelcomePage::validatePage() {
  if (choiceGroup_->checkedId() == PAGE_IMPORT_TAPROOT) {
    const bool walletWasUnloaded =
      wizard() && wizard()->property("walletWasUnloadedForSetup").toBool();
    const QString activeWalletBeforeSetup =
      wizard() ? wizard()->property("setupPreviousWalletName").toString().trimmed() : QString();

    if (walletWasUnloaded || activeWalletBeforeSetup.isEmpty()) {
      QMessageBox::information(
        this,
        "Wallet Required",
        "Taproot descriptor import uses the currently loaded wallet.\n\n"
        "Load a wallet first, or cancel this setup flow and reopen it when you are ready to import."
      );
      return false;
    }
    return true;
  }

  QString normalizedWalletName;
  const QString validationError = validateWalletNameInput(edtWalletName_->text(), &normalizedWalletName);
  if (!validationError.isEmpty()) {
    QMessageBox::warning(this, "Wallet Name", validationError);
    edtWalletName_->setFocus();
    edtWalletName_->selectAll();
    return false;
  }

  edtWalletName_->setText(normalizedWalletName);

  if (wizard()) {
    const QString provisionedWalletName = wizard()->property("provisionedWalletName").toString();
    if (!provisionedWalletName.isEmpty()) {
      if (normalizedWalletName != provisionedWalletName) {
        QMessageBox::warning(
          this,
          "Wallet Already Provisioned",
          QString("This setup session already provisioned wallet '%1'. Finish or cancel the wizard to choose a different wallet.")
            .arg(provisionedWalletName)
        );
        return false;
      }
      return true;
    }
  }

  QStringList existingWallets;
  QString walletListError;
  if (!fetchExistingWalletNames(rpc_, &existingWallets, &walletListError)) {
    // Don't block wallet creation — just skip the duplicate check.
    // The daemon will reject duplicates anyway.
    qWarning() << "Could not fetch wallet list:" << walletListError;
  }

  if (existingWallets.contains(normalizedWalletName)) {
    QMessageBox::warning(
      this,
      "Wallet Already Exists",
      QString("A wallet named '%1' already exists.\n\nChoose a different wallet name or load the existing wallet from the selector.")
        .arg(normalizedWalletName)
    );
    edtWalletName_->setFocus();
    edtWalletName_->selectAll();
    return false;
  }

  const bool walletWasUnloaded =
    wizard() && wizard()->property("walletWasUnloadedForSetup").toBool();
  const QString activeWalletBeforeSetup =
    wizard() ? wizard()->property("setupPreviousWalletName").toString().trimmed() : QString();

  if (!walletWasUnloaded && !activeWalletBeforeSetup.isEmpty()) {
    const auto reply = QMessageBox::question(
      this,
      "Unload Current Wallet",
      QString("Wallet '%1' is currently loaded.\n\n"
              "Dinero will unload it before create/restore so balances, addresses, and pending state cannot mix across wallets.\n\n"
              "Unload '%1' and continue?")
        .arg(activeWalletBeforeSetup),
      QMessageBox::Yes | QMessageBox::Cancel,
      QMessageBox::Cancel
    );
    if (reply != QMessageBox::Yes) {
      return false;
    }

    QString unloadError;
    if (!unloadWalletForProvisioning(rpc_, activeWalletBeforeSetup, &unloadError)) {
      // Don't block — the daemon will close the old wallet when opening the new one.
      qWarning() << "Could not unload" << activeWalletBeforeSetup << ":" << unloadError;
    }

    if (wizard()) {
      wizard()->setProperty("walletWasUnloadedForSetup", true);
    }
  }

  return true;
}

int WelcomePage::nextId() const {
  return choiceGroup_->checkedId();
}

// ============================================================================
// Page 2: Create New Wallet - Display Seed
// ============================================================================
CreateSeedPage::CreateSeedPage(ConnectionManager* connMgr, QWidget* parent)
    : QWizardPage(parent)
    , connMgr_(connMgr)
    , rpcTimeout_(new QTimer(this))
    , seedGenerated_(false)
    , seedVisible_(false) {

  // Configure timeout timer (30 seconds)
  rpcTimeout_->setSingleShot(true);
  rpcTimeout_->setInterval(30000);
  setTitle("Your Seed Phrase");
  setSubTitle("Write down these 12 words in order. Keep them safe and NEVER share them.");
  
  auto* layout = new QVBoxLayout;
  
  // Warning banner
  auto* warning = new QLabel(
    "⚠️ <b>CRITICAL:</b> Anyone with these words can access your funds. "
    "Write them on paper and store securely. Never take screenshots or save digitally."
  );
  warning->setStyleSheet("QLabel { background: #ff6b6b; color: white; padding: 8px; border-radius: 4px; }");
  warning->setWordWrap(true);
  layout->addWidget(warning);
  
  // Seed display (shown automatically once generated)
  lblSeed_ = new QLabel("[Seed phrase will appear here]");
  lblSeed_->setAlignment(Qt::AlignCenter);
  lblSeed_->setStyleSheet(
    "QLabel { "
    "  background: #2b2b2b; "
    "  color: #00ff00; "
    "  padding: 20px; "
    "  font-family: monospace; "
    "  font-size: 14px; "
    "  border-radius: 8px; "
    "  min-height: 200px; "
    "}"
  );
  lblSeed_->setWordWrap(true);
  layout->addWidget(lblSeed_);
  
  // Simple reveal/hide toggle (no press-and-hold)
  btnReveal_ = new QPushButton("👁️ Reveal Seed");
  btnReveal_->setStyleSheet("QPushButton { padding: 12px; font-weight: bold; }");
  btnReveal_->setEnabled(false);
  connect(btnReveal_, &QPushButton::clicked, this, &CreateSeedPage::onRevealClicked);
  layout->addWidget(btnReveal_);
  
  // Compatibility info
  auto* compatGroup = new QGroupBox("📱 Seed & Address Compatibility");
  auto* compatLayout = new QVBoxLayout(compatGroup);
  compatGroup->setStyleSheet("QGroupBox { background: #e7f5ff; padding: 10px; border-radius: 8px; }");
  
  auto* compatInfo = new QLabel(
    "<b>Your BIP39 seed phrase works across Dinero wallets.</b><br><br>"
    "✅ <b>Taproot lane:</b> BIP86 <code>din1p...</code> addresses for mobile-friendly payments<br>"
    "✅ <b>Quantum-safe lane:</b> purpose 88 P2MR <code>din1r...</code> addresses using ML-DSA-65 in Qt<br><br>"
    "Mobile restore uses the same seed for Taproot payments. P2MR keys derive from that same seed as mobile support expands.<br><br>"
    "<i>One seed phrase, clear address lanes.</i>"
  );
  compatInfo->setWordWrap(true);
  compatLayout->addWidget(compatInfo);
  layout->addWidget(compatGroup);
  
  // Confirmation checkbox
  chkCopied_ = new QCheckBox("✅ I have written down my seed phrase on paper");
  chkCopied_->setStyleSheet("QCheckBox { font-weight: bold; }");
  layout->addWidget(chkCopied_);
  
  registerField("seedConfirmed", chkCopied_);
  
  layout->addStretch();
  setLayout(layout);
}

void CreateSeedPage::initializePage() {
  const QString walletName = currentWizardWalletName(wizard());
  if (!walletName.isEmpty()) {
    setSubTitle(QString("Write down these 12 words for wallet '%1'. Keep them safe and NEVER share them.")
                  .arg(walletName));
  }

  // Generate seed on first display
  if (!seedGenerated_) {
    onGenerateSeed();
  }
}

void CreateSeedPage::onGenerateSeed() {
  // 🛡️ Bulletproof wallet creation via ConnectionManager
  if (!connMgr_) {
    lblSeed_->setText("❌ Error: Connection manager not available");
    return;
  }
  
  seedPhrase_.clear();
  seedGenerated_ = false;
  seedVisible_ = false;
  lblSeed_->setProperty("actualSeed", QString());
  btnReveal_->setEnabled(false);
  btnReveal_->setText("👁️ Reveal Seed");

  const QString walletName = currentWizardWalletName(wizard());
  if (walletName.isEmpty()) {
    lblSeed_->setText("❌ Error: Wallet name is missing. Go back and choose a wallet name first.");
    lblSeed_->setStyleSheet("QLabel { color: #ff6b6b; background: #ffe3e3; padding: 20px; }");
    return;
  }

  // Show generating status
  lblSeed_->setText(QString("🔄 Generating secure seed phrase for wallet '%1'...").arg(walletName));
  lblSeed_->setStyleSheet(
    "QLabel { "
    "  background: #2b2b2b; "
    "  color: #f59f00; "
    "  padding: 20px; "
    "  font-family: monospace; "
    "  font-size: 14px; "
    "  border-radius: 8px; "
    "  min-height: 200px; "
    "}"
  );

  // Prepare parameters for wallet.createhd
  QJsonObject params;
  params["name"] = walletName;
  params["word_count"] = 12;
  if (auto* walletWizard = qobject_cast<WalletWizard*>(wizard())) {
    walletWizard->setProperty("walletRollbackCandidateName", walletName);
    walletWizard->setProperty("provisionedWalletName", QString());
    walletWizard->setProperty("provisionedMode", QString());
  }

  // Start timeout timer (30 seconds)
  rpcTimeout_->start();
  connect(rpcTimeout_, &QTimer::timeout, this, [this]() {
    lblSeed_->setText("⏱️ Request timed out after 30 seconds.\n\n"
                      "Please check that:\n"
                      "• Daemon is running\n"
                      "• ConnectionManager is connected\n\n"
                      "Click 'Generate Seed' to try again.");
    lblSeed_->setStyleSheet("QLabel { color: #ff6b6b; background: #ffe3e3; padding: 20px; }");
    btnReveal_->setEnabled(false);
    qWarning() << "CreateSeedPage: RPC call timed out after 30 seconds";
  }, Qt::SingleShotConnection);

  // Call via ConnectionManager (automatically queued if daemon not ready)
  connMgr_->callNamed("wallet.createhd", params,
    // Success callback
    [this, walletName](QJsonObject result) {
      // Stop timeout timer
      rpcTimeout_->stop();

      QJsonObject payload = result;
      if (payload.contains("result") && payload.value("result").isObject()) {
        payload = payload.value("result").toObject();
      }

      const QString payloadError = payload.value("error").toString().trimmed();
      const bool payloadSuccess = !payload.contains("success") || payload.value("success").toBool(false);

      if (!payloadError.isEmpty() || !payloadSuccess) {
        const QString errorText = !payloadError.isEmpty()
          ? payloadError
          : QStringLiteral("Wallet creation failed.");
        QString rollbackError;
        rollbackProvisionedWallet(wizard(), &rollbackError);
        lblSeed_->setText("❌ Error generating seed: " + errorText +
                          (rollbackError.isEmpty()
                            ? QString()
                            : "\n\nRollback warning:\n" + rollbackError));
        lblSeed_->setStyleSheet("QLabel { color: #ff6b6b; background: #ffe3e3; padding: 20px; }");
        btnReveal_->setEnabled(false);
        qWarning() << "CreateSeedPage: wallet.createhd returned application error:"
                   << QJsonDocument(payload).toJson(QJsonDocument::Compact);
        return;
      }

      // Extract mnemonic, fingerprint, and first address
      QString mnemonic;
      if (payload.contains("mnemonic")) {
        mnemonic = payload["mnemonic"].toString();
      } else if (payload.contains("seed_phrase")) {
        mnemonic = payload["seed_phrase"].toString();
      } else if (payload.contains("seedPhrase")) {
        mnemonic = payload["seedPhrase"].toString();
      }

      if (!mnemonic.trimmed().isEmpty()) {
        seedPhrase_ = mnemonic.trimmed();
        seedGenerated_ = true;
        lblSeed_->setProperty("actualSeed", seedPhrase_);
        btnReveal_->setEnabled(true);

        qDebug() << "CreateSeedPage: Seed generated successfully";
        qDebug() << "CreateSeedPage: Mnemonic word count:" << seedPhrase_.split(" ").count();

        // Store fingerprint and first address for completion page
        if (payload.contains("fingerprint")) {
          setProperty("fingerprint", payload["fingerprint"].toString());
        }
        if (payload.contains("first_address")) {
          setProperty("firstAddress", payload["first_address"].toString());
        }
        if (payload.contains("wallet_name")) {
          setProperty("walletName", payload["wallet_name"].toString());
        } else {
          setProperty("walletName", walletName);
        }
        if (auto* walletWizard = qobject_cast<WalletWizard*>(wizard())) {
          walletWizard->setProperty("provisionedWalletName", walletName);
          walletWizard->setProperty("provisionedMode", QStringLiteral("create"));
        }

        // Show seed immediately after generation.
        lblSeed_->setStyleSheet(
          "QLabel { "
          "  background: #2b2b2b; "
          "  color: #00ff00; "
          "  padding: 20px; "
          "  font-family: monospace; "
          "  font-size: 14px; "
          "  border-radius: 8px; "
          "  min-height: 200px; "
          "}"
        );
        setSeedVisible(true);
      } else {
        qWarning() << "CreateSeedPage: wallet.createhd returned without mnemonic:"
                   << QJsonDocument(payload).toJson(QJsonDocument::Compact);

        // Fallback: export mnemonic from the just-created active wallet if runtime
        // state has it but createhd omitted the field.
        connMgr_->callNamed("wallet.exportseed", QJsonObject(),
          [this](QJsonObject exportResult) {
            QJsonObject exportPayload = exportResult;
            if (exportPayload.contains("result") && exportPayload.value("result").isObject()) {
              exportPayload = exportPayload.value("result").toObject();
            }

            const QString exportedMnemonic = exportPayload.value("mnemonic").toString().trimmed();
            if (exportedMnemonic.isEmpty()) {
              const QString exportError = exportPayload.value("error").toString().trimmed();
              QString rollbackError;
              rollbackProvisionedWallet(wizard(), &rollbackError);
              lblSeed_->setText("❌ Error: Wallet was created, but no seed phrase was returned.\n\n"
                                + (exportError.isEmpty()
                                   ? QStringLiteral("Mnemonic export fallback also returned no seed.")
                                   : exportError)
                                + (rollbackError.isEmpty()
                                   ? QString()
                                   : "\n\nRollback warning:\n" + rollbackError));
              lblSeed_->setStyleSheet("QLabel { color: #ff6b6b; background: #ffe3e3; padding: 20px; }");
              btnReveal_->setEnabled(false);
              return;
            }

            seedPhrase_ = exportedMnemonic;
            seedGenerated_ = true;
            seedVisible_ = false;
            lblSeed_->setProperty("actualSeed", seedPhrase_);
            btnReveal_->setEnabled(true);
            lblSeed_->setStyleSheet(
              "QLabel { "
              "  background: #2b2b2b; "
              "  color: #00ff00; "
              "  padding: 20px; "
              "  font-family: monospace; "
              "  font-size: 14px; "
              "  border-radius: 8px; "
              "  min-height: 200px; "
              "}"
            );
            setSeedVisible(true);
          },
          [this](QString exportError) {
            QString rollbackError;
            rollbackProvisionedWallet(wizard(), &rollbackError);
            lblSeed_->setText("❌ Error: Wallet was created, but no seed phrase was returned.\n\n"
                              "Mnemonic export fallback failed: " + exportError
                              + (rollbackError.isEmpty()
                                 ? QString()
                                 : "\n\nRollback warning:\n" + rollbackError));
            lblSeed_->setStyleSheet("QLabel { color: #ff6b6b; background: #ffe3e3; padding: 20px; }");
            btnReveal_->setEnabled(false);
          }
        );
      }
    },
    // Error callback
    [this](QString error) {
      // Stop timeout timer
      rpcTimeout_->stop();

      QString rollbackError;
      rollbackProvisionedWallet(wizard(), &rollbackError);
      lblSeed_->setText("❌ Error generating seed: " + error + "\n\nPlease ensure daemon is running."
                        + (rollbackError.isEmpty()
                           ? QString()
                           : "\n\nRollback warning:\n" + rollbackError));
      lblSeed_->setStyleSheet("QLabel { color: #ff6b6b; background: #ffe3e3; padding: 20px; }");
      btnReveal_->setEnabled(false);
      qWarning() << "CreateSeedPage: Failed to generate seed:" << error;
    }
  );
}

QString CreateSeedPage::formatSeedPhrase(const QString& seed) const {
  QStringList words = seed.split(" ", Qt::SkipEmptyParts);
  QString formatted;
  for (int i = 0; i < words.size(); ++i) {
    formatted += QString("%1. %2    ").arg(i + 1, 2).arg(words[i]);
    if ((i + 1) % 4 == 0) {
      formatted += "\n";
    }
  }
  return formatted.trimmed();
}

void CreateSeedPage::setSeedVisible(bool visible) {
  const QString seed = lblSeed_->property("actualSeed").toString();
  if (seed.isEmpty()) {
    seedVisible_ = false;
    lblSeed_->setText("[Seed phrase unavailable]");
    btnReveal_->setText("👁️ Reveal Seed");
    btnReveal_->setEnabled(false);
    return;
  }

  seedVisible_ = visible;
  if (seedVisible_) {
    lblSeed_->setText(formatSeedPhrase(seed));
    btnReveal_->setText("🙈 Hide Seed");
  } else {
    lblSeed_->setText("[Seed hidden. Click Reveal Seed]");
    btnReveal_->setText("👁️ Reveal Seed");
  }
}

void CreateSeedPage::onRevealClicked() {
  setSeedVisible(!seedVisible_);
}

bool CreateSeedPage::validatePage() {
  if (seedPhrase_.trimmed().isEmpty()) {
    QMessageBox::warning(this, "Seed Not Ready",
      "Seed phrase is not available yet.\n\n"
      "Wait for seed generation to complete before continuing.");
    return false;
  }

  if (!chkCopied_->isChecked()) {
    QMessageBox::warning(this, "Backup Required", 
      "You must write down your seed phrase before continuing.\n\n"
      "Without this backup, you cannot recover your wallet if your computer is lost or damaged.");
    return false;
  }
  return true;
}

int CreateSeedPage::nextId() const {
  return WalletWizard::Page_ConfirmSeed;
}

// ============================================================================
// Page 3: Confirm Seed
// ============================================================================
ConfirmSeedPage::ConfirmSeedPage(QWidget* parent) 
    : QWizardPage(parent)
    , index1_(0)
    , index2_(0)
    , index3_(0) {
  setTitle("Confirm Your Seed Phrase");
  setSubTitle("To ensure you wrote it down correctly, please enter 3 random words:");
  
  auto* layout = new QVBoxLayout;
  
  lblPrompt_ = new QLabel();
  lblPrompt_->setStyleSheet("QLabel { font-weight: bold; font-size: 14px; }");
  layout->addWidget(lblPrompt_);
  
  auto* grid = new QGridLayout;
  
  edtWord1_ = new QLineEdit;
  edtWord2_ = new QLineEdit;
  edtWord3_ = new QLineEdit;
  
  edtWord1_->setPlaceholderText("Enter word...");
  edtWord2_->setPlaceholderText("Enter word...");
  edtWord3_->setPlaceholderText("Enter word...");
  
  grid->addWidget(new QLabel("Word #"), 0, 0);
  grid->addWidget(edtWord1_, 0, 1);
  grid->addWidget(new QLabel("Word #"), 1, 0);
  grid->addWidget(edtWord2_, 1, 1);
  grid->addWidget(new QLabel("Word #"), 2, 0);
  grid->addWidget(edtWord3_, 2, 1);
  
  layout->addLayout(grid);
  layout->addStretch();
  setLayout(layout);
}

void ConfirmSeedPage::initializePage() {
  // Pick 3 random word indices (1-12 for 12-word seed)
  auto* gen = QRandomGenerator::global();
  index1_ = gen->bounded(1, 13);  // bounded(min, max) with exclusive max
  index2_ = gen->bounded(1, 13);
  index3_ = gen->bounded(1, 13);
  
  // Ensure unique indices
  while (index2_ == index1_) index2_ = gen->bounded(1, 13);
  while (index3_ == index1_ || index3_ == index2_) index3_ = gen->bounded(1, 13);
  
  // Update prompts
  qobject_cast<QLabel*>(layout()->itemAt(1)->layout()->itemAt(0)->widget())->setText(QString("Word #%1:").arg(index1_));
  qobject_cast<QLabel*>(layout()->itemAt(1)->layout()->itemAt(2)->widget())->setText(QString("Word #%2:").arg(index2_));
  qobject_cast<QLabel*>(layout()->itemAt(1)->layout()->itemAt(4)->widget())->setText(QString("Word #%3:").arg(index3_));
  
  lblPrompt_->setText(QString("Please enter words #%1, #%2, and #%3 from your seed phrase:")
    .arg(index1_).arg(index2_).arg(index3_));
  
  edtWord1_->clear();
  edtWord2_->clear();
  edtWord3_->clear();
}

bool ConfirmSeedPage::validatePage() {
  // Get the seed from the previous page
  auto* createPage = qobject_cast<CreateSeedPage*>(wizard()->page(WalletWizard::Page_CreateSeed));
  if (!createPage) return false;
  
  QStringList words = createPage->getSeedPhrase().split(" ");
  if (words.size() != 12) return false;  // 12-word seed
  
  QString word1 = words[index1_ - 1];
  QString word2 = words[index2_ - 1];
  QString word3 = words[index3_ - 1];
  
  if (edtWord1_->text().trimmed().toLower() != word1 ||
      edtWord2_->text().trimmed().toLower() != word2 ||
      edtWord3_->text().trimmed().toLower() != word3) {
    QMessageBox::warning(this, "Incorrect Words",
      "One or more words don't match your seed phrase.\n\n"
      "Please go back and write down your seed phrase carefully.");
    return false;
  }
  
  return true;
}

int ConfirmSeedPage::nextId() const {
  return WalletWizard::Page_SetPassword;
}

// ============================================================================
// Page 4: Restore from Seed
// ============================================================================
RestoreSeedPage::RestoreSeedPage(QWidget* parent) : QWizardPage(parent) {
  setTitle("Restore Wallet from Seed");
  setSubTitle("Create a named wallet from an existing BIP-39 seed phrase");
  
  auto* layout = new QVBoxLayout;
  
  // Compatibility info at top
  auto* compatInfo = new QLabel(
    "⚠️ <b>Recovery only:</b> Restore creates a new named wallet from your seed phrase.<br><br>"
    "Existing wallet names cannot be overwritten from this wizard.<br><br>"
    "📱 Import from iOS Wallet: enter your 12-word seed phrase below only when migrating/recovering."
  );
  compatInfo->setWordWrap(true);
  compatInfo->setStyleSheet("QLabel { background: #e7f5ff; padding: 10px; border-radius: 4px; }");
  layout->addWidget(compatInfo);
  
  auto* lblInstructions = new QLabel(
    "Enter your seed phrase below (one word per line or all on one line, separated by spaces):"
  );
  lblInstructions->setWordWrap(true);
  layout->addWidget(lblInstructions);
  
  txtSeed_ = new QTextEdit;
  txtSeed_->setPlaceholderText(
    "Example:\n"
    "abandon ability able about above absent absorb abstract absurd abuse access accident...\n\n"
    "Or one word per line:\n"
    "abandon\n"
    "ability\n"
    "able\n"
    "..."
  );
  txtSeed_->setMaximumHeight(150);
  connect(txtSeed_, &QTextEdit::textChanged, this, &RestoreSeedPage::onSeedChanged);
  layout->addWidget(txtSeed_);
  
  lblStatus_ = new QLabel();
  lblStatus_->setWordWrap(true);
  layout->addWidget(lblStatus_);
  
  // Optional BIP-39 passphrase (25th word)
  auto* passphraseGroup = new QGroupBox("Optional: BIP-39 Passphrase (\"25th word\")");
  auto* passphraseLayout = new QVBoxLayout(passphraseGroup);
  
  auto* lblPassphrase = new QLabel(
    "Advanced users only. Leave blank if you didn't use a passphrase when creating the wallet."
  );
  lblPassphrase->setWordWrap(true);
  lblPassphrase->setStyleSheet("QLabel { font-size: 11px; color: #888; }");
  passphraseLayout->addWidget(lblPassphrase);
  
  edtPassphrase_ = new QLineEdit;
  edtPassphrase_->setEchoMode(QLineEdit::Password);
  edtPassphrase_->setPlaceholderText("Leave empty if not used");
  passphraseLayout->addWidget(edtPassphrase_);

  // Checksum bypass for recovery of wallets with invalid checksums
  chkSkipChecksum_ = new QCheckBox("Skip BIP39 checksum validation (for recovery of old wallets)");
  chkSkipChecksum_->setStyleSheet("QCheckBox { font-size: 11px; color: #e67700; }");
  chkSkipChecksum_->setToolTip("Enable this if your seed phrase was created by an older version of Dinero that may have had a checksum bug.");
  passphraseLayout->addWidget(chkSkipChecksum_);
  
  layout->addWidget(passphraseGroup);
  layout->addStretch();
  setLayout(layout);
}

QString RestoreSeedPage::getSeedPhrase() const {
  // Normalize: remove extra whitespace, newlines, etc.
  QString text = txtSeed_->toPlainText();
  text = text.simplified(); // Converts all whitespace to single spaces
  return text;
}

void RestoreSeedPage::onSeedChanged() {
  QString seed = getSeedPhrase();
  
  if (seed.isEmpty()) {
    lblStatus_->setText("");
    return;
  }
  
  if (validateBIP39Seed(seed)) {
    lblStatus_->setText("✅ Format looks valid - Full validation on Next >");
    lblStatus_->setStyleSheet("QLabel { color: #51cf66; font-weight: bold; }");
  } else {
    lblStatus_->setText("❌ Invalid seed phrase (must be 12/15/18/21/24 valid BIP-39 words)");
    lblStatus_->setStyleSheet("QLabel { color: #ff6b6b; font-weight: bold; }");
  }
}

bool RestoreSeedPage::validateBIP39Seed(const QString& seed) {
  QStringList words = seed.split(" ", Qt::SkipEmptyParts);
  
  // BIP39 valid lengths: 12, 15, 18, 21, 24
  if (words.size() != 12 &&
      words.size() != 15 &&
      words.size() != 18 &&
      words.size() != 21 &&
      words.size() != 24) {
    return false;
  }
  
  // Basic validation: BIP-39 words are lowercase English words (3-8 letters)
  // This provides quick feedback before the RPC call validates the actual wordlist
  QRegularExpression wordPattern("^[a-z]{3,8}$");
  for (const QString& word : words) {
    if (!wordPattern.match(word).hasMatch()) {
      return false;
    }
  }
  
  // Note: Full BIP-39 wordlist and checksum validation is performed by the
  // daemon when wallet.restore RPC is called in validatePage()
  return true;
}

bool RestoreSeedPage::validatePage() {
  QString seed = getSeedPhrase();
  auto* walletWizard = qobject_cast<WalletWizard*>(this->wizard());
  const QString walletName = currentWizardWalletName(walletWizard);

  if (walletName.isEmpty()) {
    QMessageBox::warning(this, "Wallet Name",
      "Go back and choose a wallet name before restoring.");
    return false;
  }

  if (walletWizard &&
      walletWizard->property("provisionedWalletName").toString() == walletName &&
      walletWizard->property("provisionedMode").toString() == QStringLiteral("restore")) {
    lblStatus_->setText(QString("✅ Wallet '%1' already restored in this setup session").arg(walletName));
    return true;
  }

  const auto confirmRestore = QMessageBox::warning(
    this,
    "Restore Wallet Confirmation",
    QString("Restore seed phrase into wallet '%1'?\n\n"
            "This creates a new named wallet for migration or recovery.\n"
            "If you already have this wallet locally, cancel and use Load + Unlock + Rescan instead.\n\n"
            "Continue with restore?")
      .arg(walletName),
    QMessageBox::Yes | QMessageBox::Cancel,
    QMessageBox::Cancel);
  if (confirmRestore != QMessageBox::Yes) {
    return false;
  }
  
  if (!validateBIP39Seed(seed)) {
    QMessageBox::warning(this, "Invalid Seed",
      "The seed phrase you entered is not valid.\n\n"
      "Please check that you have entered a valid 12/15/18/21/24-word BIP-39 seed phrase.");
    return false;
  }
  
  // Call wallet.restore RPC to actually restore the wallet
  if (walletWizard && walletWizard->rpcClient()) {
    // Show progress
    lblStatus_->setText(QString("🔄 Restoring wallet '%1'...").arg(walletName));
    
    // Get BIP39 passphrase if provided
    QString passphrase = edtPassphrase_->text();
    
    // Call RPC
    QJsonObject params;
    params["name"] = walletName;
    params["mnemonic"] = seed;
    params["passphrase"] = passphrase.isEmpty() ? QString() : passphrase;
    params["password"] = QString();
    params["policy"] = QStringLiteral("bip86");
    params["expected_first_address"] = QString();
    params["skip_checksum"] = chkSkipChecksum_->isChecked();
    walletWizard->setProperty("walletRollbackCandidateName", walletName);
    
    // Make synchronous call and wait for result
    QEventLoop loop;
    bool success = false;
    
    connect(walletWizard->rpcClient(), &RpcClient::rpcResult, &loop,
      [this, walletWizard, walletName, &loop, &success](const QString& method, const QJsonValue& result) {
        if (method != "wallet.restore") return;
        
        if (result.isObject()) {
          auto obj = result.toObject();
          if (obj.contains("success") && obj["success"].toBool()) {
            // Store fingerprint and first address
            if (obj.contains("fingerprint")) {
              setProperty("fingerprint", obj["fingerprint"].toString());
            }
            if (obj.contains("first_address")) {
              setProperty("firstAddress", obj["first_address"].toString());
            }
            if (obj.contains("wallet_name")) {
              setProperty("walletName", obj["wallet_name"].toString());
            } else {
              setProperty("walletName", walletName);
            }
            walletWizard->setProperty("provisionedWalletName", walletName);
            walletWizard->setProperty("provisionedMode", QStringLiteral("restore"));
            success = true;
            lblStatus_->setText(QString("✅ Wallet '%1' restored successfully").arg(walletName));
          } else {
            lblStatus_->setText("❌ Restore failed: " + obj["error"].toString());
          }
        }
        loop.quit();
      });

    connect(walletWizard->rpcClient(), &RpcClient::rpcError, &loop,
      [this, &loop](const QString& method, int code, const QString& message) {
        if (method != "wallet.restore") return;
        Q_UNUSED(code);
        lblStatus_->setText(QString("❌ Restore error: %1").arg(message));
        loop.quit();
      });

    walletWizard->rpcClient()->callNamed("wallet.restore", params);
    
    // Wait for RPC to complete (with timeout)
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    
    return success;
  }
  
  return true;
}

int RestoreSeedPage::nextId() const {
  return WalletWizard::Page_SetPassword;
}

// ============================================================================
// Page 5: Set Password
// ============================================================================
SetPasswordPage::SetPasswordPage(QWidget* parent) : QWizardPage(parent) {
  setTitle("Encrypt Your Wallet");
  setSubTitle("Set a strong password to encrypt your wallet file");
  
  auto* layout = new QVBoxLayout;
  
  auto* info = new QLabel(
    "This password encrypts your wallet file using AES-256-GCM with Argon2id key derivation.\n"
    "You'll need this password to unlock your wallet and send coins."
  );
  info->setWordWrap(true);
  info->setStyleSheet("QLabel { margin-bottom: 10px; }");
  layout->addWidget(info);
  
  auto* grid = new QGridLayout;
  
  grid->addWidget(new QLabel("Password:"), 0, 0);
  edtPassword1_ = new QLineEdit;
  edtPassword1_->setEchoMode(QLineEdit::Password);
  edtPassword1_->setPlaceholderText("Enter a strong password");
  connect(edtPassword1_, &QLineEdit::textChanged, this, &SetPasswordPage::onPasswordChanged);
  grid->addWidget(edtPassword1_, 0, 1);
  
  grid->addWidget(new QLabel("Confirm:"), 1, 0);
  edtPassword2_ = new QLineEdit;
  edtPassword2_->setEchoMode(QLineEdit::Password);
  edtPassword2_->setPlaceholderText("Re-enter password");
  connect(edtPassword2_, &QLineEdit::textChanged, this, &SetPasswordPage::onPasswordChanged);
  grid->addWidget(edtPassword2_, 1, 1);
  
  layout->addLayout(grid);
  
  lblStrength_ = new QLabel();
  layout->addWidget(lblStrength_);
  
  lblMatch_ = new QLabel();
  layout->addWidget(lblMatch_);
  
  auto* warning = new QLabel(
    "⚠️ <b>Important:</b> If you forget this password, you'll need your seed phrase to restore your wallet."
  );
  warning->setWordWrap(true);
  warning->setStyleSheet("QLabel { background: #fab005; padding: 8px; border-radius: 4px; margin-top: 10px; }");
  layout->addWidget(warning);
  
  layout->addStretch();
  setLayout(layout);
}

void SetPasswordPage::onPasswordChanged() {
  QString password = edtPassword1_->text();
  QString confirm = edtPassword2_->text();
  
  // Update strength indicator
  if (!password.isEmpty()) {
    lblStrength_->setText(calculatePasswordStrength(password));
  } else {
    lblStrength_->setText("");
  }
  
  // Update match indicator
  if (!confirm.isEmpty()) {
    if (password == confirm) {
      lblMatch_->setText("✅ Passwords match");
      lblMatch_->setStyleSheet("QLabel { color: #51cf66; }");
    } else {
      lblMatch_->setText("❌ Passwords don't match");
      lblMatch_->setStyleSheet("QLabel { color: #ff6b6b; }");
    }
  } else {
    lblMatch_->setText("");
  }
}

QString SetPasswordPage::calculatePasswordStrength(const QString& password) {
  int score = 0;
  
  if (password.length() >= 8) score++;
  if (password.length() >= 12) score++;
  if (password.length() >= 16) score++;
  if (password.contains(QRegularExpression("[a-z]"))) score++;
  if (password.contains(QRegularExpression("[A-Z]"))) score++;
  if (password.contains(QRegularExpression("[0-9]"))) score++;
  if (password.contains(QRegularExpression("[^A-Za-z0-9]"))) score++;
  
  QString strength;
  QString color;
  
  if (score < 3) {
    strength = "Weak ⚠️";
    color = "#ff6b6b";
  } else if (score < 5) {
    strength = "Fair";
    color = "#fab005";
  } else if (score < 6) {
    strength = "Good";
    color = "#51cf66";
  } else {
    strength = "Strong ✅";
    color = "#51cf66";
  }
  
  QString html = QString("Password strength: <span style='color: %1; font-weight: bold;'>%2</span>")
    .arg(color).arg(strength);
  
  return html;
}

bool SetPasswordPage::validatePage() {
  QString password = edtPassword1_->text();
  QString confirm = edtPassword2_->text();
  
  if (password.length() < 8) {
    QMessageBox::warning(this, "Weak Password",
      "Password must be at least 8 characters long.\n\n"
      "For security, use a strong password with letters, numbers, and symbols.");
    return false;
  }
  
  if (password != confirm) {
    QMessageBox::warning(this, "Password Mismatch",
      "The passwords you entered don't match.\n\nPlease try again.");
    edtPassword2_->clear();
    edtPassword2_->setFocus();
    return false;
  }
  
  return true;
}

int SetPasswordPage::nextId() const {
  return WalletWizard::Page_Completion;
}

// ============================================================================
// Page 6: Completion
// ============================================================================
CompletionPage::CompletionPage(RpcClient* rpc, QWidget* parent) 
    : QWizardPage(parent)
    , rpc_(rpc) {
  setTitle("Wallet Setup Complete");
  setSubTitle("Your Dinero wallet is ready to use");
  
  auto* layout = new QVBoxLayout;
  
  auto* success = new QLabel("✅ Your wallet setup has completed.");
  success->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; color: #51cf66; margin-bottom: 20px; }");
  layout->addWidget(success);
  
  auto* grid = new QGridLayout;

  grid->addWidget(new QLabel("Wallet Name:"), 0, 0);
  lblWalletName_ = new QLabel("-");
  lblWalletName_->setStyleSheet("QLabel { font-family: monospace; font-weight: bold; }");
  grid->addWidget(lblWalletName_, 0, 1);
  
  grid->addWidget(new QLabel("Wallet Fingerprint:"), 1, 0);
  lblFingerprint_ = new QLabel("-");
  lblFingerprint_->setStyleSheet("QLabel { font-family: monospace; font-weight: bold; }");
  grid->addWidget(lblFingerprint_, 1, 1);
  
  grid->addWidget(new QLabel("First Address:"), 2, 0);
  lblFirstAddress_ = new QLabel("-");
  lblFirstAddress_->setStyleSheet("QLabel { font-family: monospace; }");
  lblFirstAddress_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  grid->addWidget(lblFirstAddress_, 2, 1);
  
  layout->addLayout(grid);
  
  lblStatus_ = new QLabel();
  lblStatus_->setWordWrap(true);
  layout->addWidget(lblStatus_);
  
  auto* reminder = new QLabel(
    "<p><b>Important Reminders:</b></p>"
    "<ul>"
    "<li>Keep your seed phrase safe and offline</li>"
    "<li>Never share your seed phrase with anyone</li>"
    "<li>Make multiple backups stored in different locations</li>"
    "<li>Your password encrypts the wallet file, but the seed phrase is the ultimate backup</li>"
    "</ul>"
  );
  reminder->setWordWrap(true);
  reminder->setStyleSheet("QLabel { background: #e9ecef; padding: 10px; border-radius: 4px; margin-top: 20px; }");
  layout->addWidget(reminder);
  
  layout->addStretch();
  setLayout(layout);
}

void CompletionPage::initializePage() {
  lblStatus_->setText("Creating wallet...");

  // Get data from previous pages
  auto* wizard = qobject_cast<WalletWizard*>(this->wizard());
  if (!wizard) {
    lblStatus_->setText("❌ Error: Wizard not found");
    return;
  }

  // Get create/restore page data
  auto* createPage = qobject_cast<CreateSeedPage*>(wizard->page(WalletWizard::Page_CreateSeed));
  auto* restorePage = qobject_cast<RestoreSeedPage*>(wizard->page(WalletWizard::Page_RestoreSeed));
  auto* passwordPage = qobject_cast<SetPasswordPage*>(wizard->page(WalletWizard::Page_SetPassword));

  if (!createPage || !restorePage || !passwordPage) {
    lblStatus_->setText("❌ Error: Pages not found");
    return;
  }

  // Restore path detection must use wizard history; relying on WelcomePage::nextId()
  // here is brittle and can drift from actual navigation state.
  const bool isRestore = wizard->hasVisitedPage(WalletWizard::Page_RestoreSeed);

  QString fingerprint;
  QString firstAddress;
  QString walletName = currentWizardWalletName(wizard);

  if (isRestore) {
    // Wallet was already restored in RestoreSeedPage
    fingerprint = restorePage->property("fingerprint").toString();
    firstAddress = restorePage->property("firstAddress").toString();
    if (walletName.isEmpty()) {
      walletName = restorePage->property("walletName").toString();
    }

    if (fingerprint.isEmpty() || firstAddress.isEmpty()) {
      lblStatus_->setText("❌ Wallet restore failed");
      return;
    }
  } else {
    // Wallet was already created in CreateSeedPage
    fingerprint = createPage->property("fingerprint").toString();
    firstAddress = createPage->property("firstAddress").toString();
    if (walletName.isEmpty()) {
      walletName = createPage->property("walletName").toString();
    }

    if (fingerprint.isEmpty() || firstAddress.isEmpty()) {
      lblStatus_->setText("❌ Wallet creation failed");
      return;
    }
  }

  // Display wallet info
  lblWalletName_->setText(walletName.isEmpty() ? QStringLiteral("-") : walletName);
  lblFingerprint_->setText(fingerprint);
  lblFirstAddress_->setText(firstAddress);
  setProperty("fingerprint", fingerprint);
  setProperty("walletName", walletName);
  const QString walletAction = isRestore ? "restored" : "created";
  const QString walletDisplayName = walletName.isEmpty()
    ? QStringLiteral("active wallet")
    : walletName;
  lblStatus_->setText(QString("✅ Wallet '%1' %2. Encrypting...")
                        .arg(walletDisplayName, walletAction));

  // Get password and encrypt wallet automatically
  QString password = passwordPage->getPassword();

  if (password.isEmpty()) {
    lblStatus_->setText("❌ No password provided");
    return;
  }

  // Call encryptwallet RPC
  QJsonArray params;
  params.append(password);

  auto normalizeUnlockError = [](QString msg) {
    msg = msg.trimmed();
    const QString failedPrefix = "failed to unlock wallet:";
    if (msg.toLower().startsWith(failedPrefix)) {
      msg = msg.mid(failedPrefix.size()).trimmed();
    }
    msg.replace("passphrase", "password", Qt::CaseInsensitive);
    return msg;
  };

  auto resultConn = std::make_shared<QMetaObject::Connection>();
  auto errorConn = std::make_shared<QMetaObject::Connection>();

  *resultConn = connect(rpc_, &RpcClient::rpcResult, this,
    [this, password, walletAction, normalizeUnlockError, resultConn, errorConn](const QString& method, const QJsonValue& result) {
      if (method == "wallet.encrypt") {
        if (!result.isObject()) {
          QObject::disconnect(*resultConn);
          QObject::disconnect(*errorConn);
          lblStatus_->setText(QString("⚠️ Wallet %1 but encryption returned invalid response.").arg(walletAction));
          return;
        }

        const auto obj = result.toObject();
        if (obj.contains("success") && obj["success"].toBool()) {
          lblStatus_->setText(QString("✅ Wallet %1 and encrypted. Unlocking...").arg(walletAction));
          // Keep wallet open by default after setup. User can lock manually.
          rpc_->walletUnlock(password, 0);
        } else {
          const QString error = obj.contains("error") ? obj["error"].toString() : "Unknown error";
          QObject::disconnect(*resultConn);
          QObject::disconnect(*errorConn);
          lblStatus_->setText(QString("⚠️ Wallet %1 but encryption failed: %2").arg(walletAction, error));
        }
        return;
      }

      if (method == "wallet.unlock") {
        QObject::disconnect(*resultConn);
        QObject::disconnect(*errorConn);

        if (result.isObject() && result.toObject().contains("error")) {
          const QString rawError = result.toObject().value("error").toString();
          lblStatus_->setText(QString("⚠️ Wallet %1 and encrypted, but unlock failed: %2")
            .arg(walletAction, normalizeUnlockError(rawError)));
          return;
        }

        lblStatus_->setText(QString("✅ Wallet %1, encrypted, and unlocked.").arg(walletAction));
      }
    });

  *errorConn = connect(rpc_, &RpcClient::rpcError, this,
    [this, walletAction, normalizeUnlockError, resultConn, errorConn](const QString& method, int code, const QString& message) {
      Q_UNUSED(code);
      if (method != "wallet.encrypt" && method != "wallet.unlock") {
        return;
      }

      QObject::disconnect(*resultConn);
      QObject::disconnect(*errorConn);

      if (method == "wallet.encrypt") {
        lblStatus_->setText(QString("⚠️ Wallet %1 but encryption failed: %2").arg(walletAction, message));
      } else {
        lblStatus_->setText(QString("⚠️ Wallet %1 and encrypted, but unlock failed: %2")
          .arg(walletAction, normalizeUnlockError(message)));
      }
    });

  rpc_->call("wallet.encrypt", params);
}

// ============================================================================
// Main Wallet Wizard
// ============================================================================
WalletWizard::WalletWizard(ConnectionManager* connMgr, RpcClient* rpc, QWidget* parent) 
    : QWizard(parent)
    , connMgr_(connMgr)
    , rpc_(rpc) {
  setWindowTitle("Dinero Wallet Setup");
  setWizardStyle(QWizard::ModernStyle);
  setOption(QWizard::HaveHelpButton, false);
  setMinimumSize(700, 500);
  
  // Add pages
  setProperty("provisionedWalletName", QString());
  setProperty("provisionedMode", QString());
  setProperty("setupPreviousWalletName", QString());
  setProperty("walletWasUnloadedForSetup", false);
  setProperty("walletRollbackCandidateName", QString());

  setPage(Page_Welcome, new WelcomePage(rpc_, this));
  setPage(Page_CreateSeed, new CreateSeedPage(connMgr_, this));  // Uses ConnectionManager
  setPage(Page_ConfirmSeed, new ConfirmSeedPage(this));
  setPage(Page_RestoreSeed, new RestoreSeedPage(this));
  setPage(Page_SetPassword, new SetPasswordPage(this));
  setPage(Page_Completion, new CompletionPage(rpc_, this));
  setPage(Page_ImportTaproot, new ImportTaprootPage(connMgr_, this));  // Taproot import
  
  setStartId(Page_Welcome);
  
  connect(this, &QWizard::finished, this, &WalletWizard::onFinished);
}

void WalletWizard::onFinished(int result) {
  if (result == QDialog::Accepted) {
    // Emit signal that wallet was created with real fingerprint
    auto* completionPage = qobject_cast<CompletionPage*>(page(Page_Completion));
    if (completionPage) {
      // Get the real fingerprint from the completion page
      QString fingerprint = completionPage->property("fingerprint").toString();
      QString walletName = completionPage->property("walletName").toString();
      if (fingerprint.isEmpty()) {
        // Fallback: Try to get from create/restore pages
        auto* createPage = qobject_cast<CreateSeedPage*>(page(Page_CreateSeed));
        auto* restorePage = qobject_cast<RestoreSeedPage*>(page(Page_RestoreSeed));
        
        if (createPage) {
          fingerprint = createPage->property("fingerprint").toString();
          if (walletName.isEmpty()) {
            walletName = createPage->property("walletName").toString();
          }
        }
        if (fingerprint.isEmpty() && restorePage) {
          fingerprint = restorePage->property("fingerprint").toString();
        }
        if (walletName.isEmpty() && restorePage) {
          walletName = restorePage->property("walletName").toString();
        }
      }
      if (walletName.isEmpty()) {
        walletName = field("walletName").toString();
      }
      
      const bool restored = hasVisitedPage(Page_RestoreSeed);
      Q_EMIT walletCreated(walletName, fingerprint.isEmpty() ? "unknown" : fingerprint, restored);
    }
    return;
  }

  QString rollbackError;
  if (!rollbackProvisionedWallet(this, &rollbackError) && !rollbackError.isEmpty()) {
    QMessageBox::warning(
      this,
      "Wallet Setup Rollback Failed",
      "Dinero could not fully roll back the wallet setup session.\n\n" + rollbackError
    );
  }
}

// ============================================================================
// Page 7: Import Taproot Descriptor
// ============================================================================
ImportTaprootPage::ImportTaprootPage(ConnectionManager* connMgr, QWidget* parent)
    : QWizardPage(parent)
    , connMgr_(connMgr)
    , importSucceeded_(false) {
  setTitle("Import Taproot Descriptor");
  setSubTitle("Import a Taproot private key using descriptor format");

  auto* layout = new QVBoxLayout;

  // Warning banner
  auto* warning = new QLabel(
    "<p style='background: #ff922b; color: white; padding: 8px; border-radius: 4px;'>"
    "⚠️ <b>Advanced Feature:</b> Only use this if you have a Taproot descriptor "
    "(e.g., from mining setup or another wallet). For most users, restoring from "
    "a seed phrase is recommended.</p>"
  );
  warning->setWordWrap(true);
  layout->addWidget(warning);

  // Descriptor input
  auto* descLabel = new QLabel("<b>Taproot Descriptor:</b>");
  layout->addWidget(descLabel);

  edtDescriptor_ = new QLineEdit;
  edtDescriptor_->setPlaceholderText("tr(<64-character-hex-private-key>)");
  edtDescriptor_->setMinimumWidth(500);
  edtDescriptor_->setStyleSheet("QLineEdit { font-family: monospace; padding: 8px; }");
  connect(edtDescriptor_, &QLineEdit::textChanged, this, &ImportTaprootPage::onDescriptorChanged);
  layout->addWidget(edtDescriptor_);

  // Format help
  auto* formatHelp = new QLabel(
    "<p style='font-size: 11px; color: #666;'>"
    "Format: <code>tr(&lt;64-hex-chars&gt;)</code><br>"
    "Example: <code>tr(0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef)</code></p>"
  );
  formatHelp->setWordWrap(true);
  layout->addWidget(formatHelp);

  // Optional label
  auto* labelLabel = new QLabel("<b>Label (optional):</b>");
  layout->addWidget(labelLabel);

  edtLabel_ = new QLineEdit;
  edtLabel_->setPlaceholderText("e.g., Mining rewards, Cold storage");
  layout->addWidget(edtLabel_);

  layout->addSpacing(10);

  // Import button
  btnImport_ = new QPushButton("Import Taproot Key");
  btnImport_->setStyleSheet(
    "QPushButton { background: #228be6; color: white; font-weight: bold; padding: 10px 20px; }"
    "QPushButton:disabled { background: #868e96; }"
  );
  btnImport_->setEnabled(false);
  connect(btnImport_, &QPushButton::clicked, this, &ImportTaprootPage::onImportClicked);
  layout->addWidget(btnImport_);

  // Status label
  lblStatus_ = new QLabel("");
  lblStatus_->setWordWrap(true);
  layout->addWidget(lblStatus_);

  // Address display (shown after successful import)
  lblAddress_ = new QLabel("");
  lblAddress_->setStyleSheet(
    "QLabel { background: #d3f9d8; color: #2b8a3e; padding: 10px; border-radius: 4px; font-family: monospace; }"
  );
  lblAddress_->setVisible(false);
  layout->addWidget(lblAddress_);

  // Note about mandatory rescan
  // Security requirement note
  auto* unlockNote = new QLabel(
    "<p style='font-size: 10px; color: #e03131; margin-top: 10px;'>"
    "<b>Security:</b> Your wallet must be <b>unlocked</b> to import private keys. "
    "If your wallet is encrypted, unlock it first from the main toolbar.</p>"
  );
  unlockNote->setWordWrap(true);
  layout->addWidget(unlockNote);

  auto* rescanNote = new QLabel(
    "<p style='font-size: 10px; color: #666;'>"
    "<b>Note:</b> Importing a Taproot descriptor will automatically trigger a blockchain "
    "rescan to find any existing transactions. This may take some time depending on "
    "blockchain size.</p>"
  );
  rescanNote->setWordWrap(true);
  layout->addWidget(rescanNote);

  layout->addStretch();
  setLayout(layout);
}

void ImportTaprootPage::initializePage() {
  // Reset state when page is shown
  importSucceeded_ = false;
  importedAddress_.clear();
  lblStatus_->clear();
  lblAddress_->setVisible(false);
  edtDescriptor_->clear();
  edtLabel_->clear();
  btnImport_->setEnabled(false);
}

bool ImportTaprootPage::validatePage() {
  // Only allow proceeding if import succeeded
  if (!importSucceeded_) {
    QMessageBox::warning(const_cast<ImportTaprootPage*>(this), "Import Required",
      "Please import a valid Taproot descriptor before continuing.\n\n"
      "Click the 'Import Taproot Key' button after entering a valid descriptor.");
    return false;
  }
  return true;
}

int ImportTaprootPage::nextId() const {
  // Skip password page for descriptor import (goes directly to completion)
  return WalletWizard::Page_Completion;
}

QString ImportTaprootPage::getDescriptor() const {
  return edtDescriptor_->text().trimmed();
}

void ImportTaprootPage::onDescriptorChanged() {
  QString desc = edtDescriptor_->text().trimmed();

  // Basic validation: tr(<64 hex chars>)
  QRegularExpression pattern("^tr\\([0-9a-fA-F]{64}\\)$");
  bool valid = pattern.match(desc).hasMatch();

  btnImport_->setEnabled(valid);

  if (!desc.isEmpty() && !valid) {
    lblStatus_->setText("<span style='color: #e03131;'>Invalid format. Expected: tr(&lt;64-hex-chars&gt;)</span>");
    lblStatus_->setStyleSheet("");
  } else {
    lblStatus_->clear();
  }

  // Reset import state when descriptor changes
  if (importSucceeded_) {
    importSucceeded_ = false;
    importedAddress_.clear();
    lblAddress_->setVisible(false);
  }
}

void ImportTaprootPage::onImportClicked() {
  QString descriptor = getDescriptor();
  QString label = getLabel();

  if (descriptor.isEmpty()) {
    lblStatus_->setText("<span style='color: #e03131;'>Please enter a Taproot descriptor</span>");
    return;
  }

  // Disable button during import
  btnImport_->setEnabled(false);
  lblStatus_->setText("<span style='color: #228be6;'>Importing... (this may take a moment for rescan)</span>");

  // Call the RPC
  QJsonObject params;
  params["descriptor"] = descriptor;
  if (!label.isEmpty()) {
    params["label"] = label;
  }

  // Use ConnectionManager for reliable RPC call
  connMgr_->callNamed("wallet.importtaprootdescriptor", params,
    // Success callback
    [this](QJsonObject result) {
      if (result["success"].toBool()) {
        importSucceeded_ = true;
        importedAddress_ = result["address"].toString();

        lblStatus_->setText(
          QString("<span style='color: #2f9e44;'><b>Success!</b> Taproot key imported. "
                  "Blockchain rescan has been triggered.</span>")
        );

        lblAddress_->setText(
          QString("<b>Imported Address:</b><br>%1<br><br>"
                  "<b>Internal Pubkey:</b> %2<br>"
                  "<b>Output Pubkey:</b> %3")
            .arg(importedAddress_)
            .arg(result["internal_pubkey"].toString())
            .arg(result["output_pubkey"].toString())
        );
        lblAddress_->setVisible(true);

        // Re-enable button but change text
        btnImport_->setText("Import Complete");
        btnImport_->setEnabled(false);

        // Notify the wizard that we can proceed
        Q_EMIT completeChanged();
      } else {
        QString error = result["message"].toString();
        if (error.isEmpty()) {
          error = result["error"].toString();
        }

        // Provide specific guidance for wallet lock errors
        if (error.contains("locked", Qt::CaseInsensitive) ||
            error.contains("unlock", Qt::CaseInsensitive)) {
          lblStatus_->setText(
            QString("<span style='color: #e03131;'><b>Wallet is locked!</b><br>"
                    "Taproot key import requires an unlocked wallet.<br>"
                    "Please unlock your wallet first (main toolbar → Unlock Wallet), then try again.</span>")
          );
        } else {
          lblStatus_->setText(
            QString("<span style='color: #e03131;'><b>Import failed:</b> %1</span>").arg(error)
          );
        }
        btnImport_->setEnabled(true);
      }
    },
    // Error callback
    [this](QString error) {
      // Provide specific guidance for wallet lock errors
      if (error.contains("locked", Qt::CaseInsensitive) ||
          error.contains("unlock", Qt::CaseInsensitive)) {
        lblStatus_->setText(
          QString("<span style='color: #e03131;'><b>Wallet is locked!</b><br>"
                  "Taproot key import requires an unlocked wallet.<br>"
                  "Please unlock your wallet first, then try again.</span>")
        );
      } else {
        lblStatus_->setText(
          QString("<span style='color: #e03131;'><b>RPC Error:</b> %1</span>").arg(error)
        );
      }
      btnImport_->setEnabled(true);
    }
  );
}
