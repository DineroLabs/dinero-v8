#pragma once

#include <QDialog>
#include <QTextEdit>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QVBoxLayout>

/**
 * Dialog for importing encrypted private keys (vault format)
 * Supports encrypted blobs with passphrase decryption and optional rescanning
 */
class ImportVaultDialog : public QDialog {
    Q_OBJECT

public:
    enum RescanMode {
        NoRescan = 0,
        FromNow = 1,
        FromHeight = 2
    };

    explicit ImportVaultDialog(QWidget* parent = nullptr);

    // Getters for dialog data
    QString blob() const;
    QString passphrase() const;
    bool doRescan() const;
    RescanMode rescanMode() const;
    int fromHeight() const;

private slots:
    void onRescanToggled(bool enabled);
    void onBlobTextChanged();
    void onPassphraseChanged();
    void validateInput();

private:
    void setupUI();
    bool isValidBlob(const QString& blob) const;

    // UI Components
    QTextEdit* m_blobEdit;
    QLineEdit* m_passphraseEdit;
    QCheckBox* m_rescanCheckBox;
    QComboBox* m_rescanModeCombo;
    QSpinBox* m_heightSpinBox;
    QDialogButtonBox* m_buttonBox;
    
    // Validation
    QLabel* m_validationLabel;
};

