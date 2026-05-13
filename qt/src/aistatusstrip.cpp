#include "aistatusstrip.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>

AiStatusStrip::AiStatusStrip(QWidget* parent)
    : QWidget(parent)
{
    setupUI();
}

void AiStatusStrip::setupUI()
{
    setFixedHeight(36);
    setCursor(Qt::PointingHandCursor);
    setStyleSheet(
        "AiStatusStrip { background: #1a1d22; border-top: 1px solid #2f343c; }");

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 0, 12, 0);
    layout->setSpacing(0);

    statusLabel_ = new QLabel("Connecting...");
    statusLabel_->setStyleSheet(
        "QLabel { color: #868e96; font-size: 12px; font-weight: 500; "
        "font-family: \"Space Mono\", \"SF Mono\", Menlo, monospace; "
        "background: transparent; border: none; }");
    layout->addWidget(statusLabel_, 1);

    // Platform-appropriate shortcut hint
#ifdef Q_OS_MAC
    QString hint = "\u2318K";  // ⌘K
#else
    QString hint = "Ctrl+K";
#endif

    shortcutHint_ = new QLabel(hint);
    shortcutHint_->setStyleSheet(
        "QLabel { color: #5c6370; font-size: 11px; font-weight: 600; "
        "font-family: \"Space Mono\", \"SF Mono\", Menlo, monospace; "
        "background: #20242a; border: 1px solid #353b44; border-radius: 4px; "
        "padding: 2px 6px; }");
    shortcutHint_->setFixedHeight(22);
    layout->addWidget(shortcutHint_);
}

void AiStatusStrip::updateStatus(int height, int headers, int peerCount,
                                  double balance, bool miningActive, double hashrateHps,
                                  bool bridgeActive, int proofCacheEntries)
{
    QStringList parts;

    // Sync status
    if (headers <= 0) {
        parts << "<span style='color:#868e96;'>Connecting...</span>";
    } else {
        double syncPct = 100.0 * height / headers;
        if (syncPct >= 99.9) {
            parts << QString("<span style='color:#51cf66;'>Synced</span> at %1")
                         .arg(QLocale().toString(height));
        } else {
            parts << QString("<span style='color:#fab005;'>Syncing</span> %1%")
                         .arg(syncPct, 0, 'f', 1);
        }
    }

    // Mining
    if (miningActive && hashrateHps > 0) {
        QString hrStr;
        if (hashrateHps >= 1e9)
            hrStr = QString("%1 GH/s").arg(hashrateHps / 1e9, 0, 'f', 2);
        else if (hashrateHps >= 1e6)
            hrStr = QString("%1 MH/s").arg(hashrateHps / 1e6, 0, 'f', 2);
        else if (hashrateHps >= 1e3)
            hrStr = QString("%1 kH/s").arg(hashrateHps / 1e3, 0, 'f', 1);
        else
            hrStr = QString("%1 H/s").arg(hashrateHps, 0, 'f', 0);
        parts << QString("<span style='color:#339af0;'>%1</span>").arg(hrStr);
    }

    // Utreexo proof service
    if (bridgeActive) {
        if (proofCacheEntries > 0)
            parts << QString("<span style='color:#51cf66;'>Proofs</span> %1 cached").arg(proofCacheEntries);
        else
            parts << "<span style='color:#51cf66;'>Proofs ready</span>";
    } else {
        parts << "<span style='color:#5c6370;'>Proofs off</span>";
    }

    // Peers
    parts << QString("%1 peer%2").arg(peerCount).arg(peerCount != 1 ? "s" : "");

    // Balance
    parts << QString("%1 DIN").arg(QLocale().toString(balance, 'f', 2));

    statusLabel_->setText(parts.join(" \u00B7 ")); // middle dot separator
}

void AiStatusStrip::mousePressEvent(QMouseEvent* event)
{
    Q_UNUSED(event)
    Q_EMIT clicked();
}
