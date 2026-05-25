// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DINERO_QT_DISCOVERYSECTION_H
#define DINERO_QT_DISCOVERYSECTION_H

#include "dashboardtypes.h"

#include <QFrame>
#include <QHash>
#include <QVector>

class QLabel;
class QVBoxLayout;

namespace dinero::qt::dashboard {

class HintRowWidget;

// Phase 2b — "you have learned about these relay paths": one row per
// relay-hint endpoint in the daemon's cache, with freshness/failure
// indicators. Receives QVector<HintRow> via setHints().
//
// Rows that are present in the previous tick but absent from the new
// tick fade out (QPropertyAnimation on opacity, 1s) before being
// removed from the layout.
class DiscoverySection : public QFrame {
    Q_OBJECT
public:
    explicit DiscoverySection(QWidget* parent = nullptr);

public Q_SLOTS:
    void setHints(const QVector<HintRow>& hints);

private:
    QLabel*           header_label_{nullptr};
    QVBoxLayout*      rows_layout_{nullptr};
    // Key: target_node_id_hex + "@" + endpoint. Identifies a unique row
    // across ticks so we can fade out evictions instead of redrawing.
    QHash<QString, HintRowWidget*> active_rows_;

    static QString rowKey(const HintRow& r);
};

}  // namespace dinero::qt::dashboard

#endif  // DINERO_QT_DISCOVERYSECTION_H
