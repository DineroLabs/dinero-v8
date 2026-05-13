#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <optional>

struct ImportRequest {
    enum class Type { Wif, Vault } type;
    QString wif;                // when type == Wif
    QString label;
    bool    rescan = false;
    std::optional<int> rescanFromHeight;
    QString encryptedBlob;      // when type == Vault
    QString passphrase;

    bool isValid(QString* outErr = nullptr) const;
};

/**
 * Dialog for importing private keys in WIF format
 * Supports optional labeling and blockchain rescanning
 */
class ImportKeyDialog : public QDialog {
    Q_OBJECT

public:
    enum RescanMode {
        NoRescan = 0,
        FromNow = 1,
        FromHeight = 2
    };

    explicit ImportKeyDialog(QWidget* parent = nullptr);

    // Modern interface
    ImportRequest buildRequest() const;
    void setBusy(bool on);
    void showInlineError(const QString& msg);
    
    // Legacy getters (for compatibility)
    QString wif() const;
    QString label() const;
    bool doRescan() const;
    RescanMode rescanMode() const;
    int fromHeight() const;

private slots:
    void onRescanToggled(bool enabled);
    void onWifTextChanged();
    void validateInput();

private:
    void setupUI();
    bool isValidWif(const QString& wif) const;

    // UI Components
    QLineEdit* m_wifEdit;
    QLineEdit* m_labelEdit;
    QCheckBox* m_rescanCheckBox;
    QComboBox* m_rescanModeCombo;
    QSpinBox* m_heightSpinBox;
    QDialogButtonBox* m_buttonBox;
    
    // Validation
    QLabel* m_validationLabel;
};

