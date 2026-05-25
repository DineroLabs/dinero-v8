// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "discoverysection.h"

#include <QFont>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPropertyAnimation>
#include <QVariant>
#include <QVBoxLayout>

#include <functional>
#include <utility>

namespace dinero::qt::dashboard {

namespace {

constexpr qint64 kAssumedTtlSeconds = 900;  // kHintTtl from daemon — TODO plumb dynamically

QString formatTargetEllipsis(const QString& hex) {
    if (hex.size() <= 10) return hex;
    return hex.left(4) + QStringLiteral("…") + hex.right(4);
}

QString glyphForState(const HintRow& r, qint64 ttl_seconds) {
    if (r.dial_failures >= 3) return QStringLiteral("✗");  // ✗
    if (r.near_eviction)      return QStringLiteral("⚠");  // ⚠
    const auto ageing_threshold_secs = ttl_seconds / 3;
    return r.age_seconds < ageing_threshold_secs
               ? QStringLiteral("●")   // ●
               : QStringLiteral("◐");  // ◐
}

}  // namespace

// --- FreshnessBar: 10-segment horizontal bar -------------------------------

class FreshnessBar : public QWidget {
public:
    explicit FreshnessBar(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(80, 10);
    }
    void setFraction(double f) { fraction_ = f; update(); }
    QSize sizeHint() const override { return {80, 10}; }

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter p(this);
        constexpr int segs = 10;
        const int filled = static_cast<int>(fraction_ * segs + 0.5);
        const int seg_w = (width() - segs + 1) / segs;
        for (int i = 0; i < segs; ++i) {
            const QColor c = i < filled ? QColor(80, 160, 240)
                                        : QColor(220, 220, 220);
            p.fillRect(i * (seg_w + 1), 0, seg_w, height(), c);
        }
    }

private:
    double fraction_{1.0};
};

// --- HintRowWidget: one row of the section --------------------------------

class HintRowWidget : public QWidget {
public:
    explicit HintRowWidget(QWidget* parent = nullptr) : QWidget(parent) {
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(2, 2, 2, 2);
        row->setSpacing(8);
        glyph_     = new QLabel(this);
        target_    = new QLabel(this);
        endpoint_  = new QLabel(this);
        freshness_ = new FreshnessBar(this);
        failures_  = new QLabel(this);
        row->addWidget(glyph_);
        row->addWidget(target_);
        row->addWidget(endpoint_, 1);
        row->addWidget(freshness_);
        row->addWidget(failures_);
    }

    void update(const HintRow& r, qint64 ttl_seconds) {
        target_->setText(formatTargetEllipsis(r.target_node_id_hex));
        endpoint_->setText(r.endpoint);
        const double freshness =
            ttl_seconds > 0 ? 1.0 - double(r.age_seconds) / double(ttl_seconds)
                            : 0.0;
        freshness_->setFraction(qBound(0.0, freshness, 1.0));
        glyph_->setText(glyphForState(r, ttl_seconds));
        if (r.near_eviction) {
            failures_->setText(QStringLiteral("%1 → evict").arg(r.dial_failures));
            failures_->setStyleSheet(QStringLiteral("color: red;"));
        } else {
            failures_->setText(QString::number(r.dial_failures));
            failures_->setStyleSheet(QString());
        }
    }

    void startFadeOut(std::function<void()> on_done) {
        auto* effect = new QGraphicsOpacityEffect(this);
        setGraphicsEffect(effect);
        auto* anim = new QPropertyAnimation(effect, "opacity", this);
        anim->setDuration(1000);
        anim->setStartValue(1.0);
        anim->setEndValue(0.0);
        QObject::connect(anim, &QPropertyAnimation::finished, this,
                         [on_done = std::move(on_done)]() { on_done(); });
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    }

private:
    QLabel*       glyph_{nullptr};
    QLabel*       target_{nullptr};
    QLabel*       endpoint_{nullptr};
    FreshnessBar* freshness_{nullptr};
    QLabel*       failures_{nullptr};
};

// --- DiscoverySection -----------------------------------------------------

DiscoverySection::DiscoverySection(QWidget* parent) : QFrame(parent) {
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Sunken);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);
    header_label_ = new QLabel(tr("Discovery — 0 targets known"), this);
    QFont f = header_label_->font();
    f.setBold(true);
    header_label_->setFont(f);
    root->addWidget(header_label_);
    rows_layout_ = new QVBoxLayout();
    rows_layout_->setSpacing(2);
    root->addLayout(rows_layout_);
    root->addStretch(1);
}

void DiscoverySection::setHints(const QVector<HintRow>& hints) {
    header_label_->setText(
        tr("Discovery — %1 targets known").arg(hints.size()));

    QHash<QString, HintRowWidget*> next_rows;
    for (const auto& r : hints) {
        const auto key = rowKey(r);
        auto it = active_rows_.find(key);
        HintRowWidget* w = nullptr;
        if (it != active_rows_.end()) {
            w = *it;
            active_rows_.erase(it);
        } else {
            w = new HintRowWidget(this);
            w->setContextMenuPolicy(Qt::CustomContextMenu);
            QObject::connect(w, &QWidget::customContextMenuRequested,
                             this, &DiscoverySection::onRowContextMenuRequested);
            rows_layout_->addWidget(w);
        }
        w->update(r, kAssumedTtlSeconds);
        w->setProperty("hintRow", QVariant::fromValue(r));
        next_rows.insert(key, w);
    }
    // Any active_rows_ remaining are evictions — fade them out.
    for (auto* w : active_rows_) {
        w->startFadeOut([w] {
            w->setParent(nullptr);
            w->deleteLater();
        });
    }
    active_rows_ = next_rows;
}

QString DiscoverySection::rowKey(const HintRow& r) {
    return r.target_node_id_hex + QStringLiteral("@") + r.endpoint;
}

void DiscoverySection::onRowContextMenuRequested(const QPoint& pos) {
    auto* row = qobject_cast<QWidget*>(sender());
    if (!row) return;
    const auto hint = row->property("hintRow").value<HintRow>();
    if (hint.target_node_id_hex.isEmpty()) return;

    QMenu menu(this);
    auto* copy_endpoint = menu.addAction(tr("Copy relay endpoint"));
    copy_endpoint->setEnabled(!hint.endpoint.isEmpty() &&
                              hint.endpoint != QStringLiteral("(no addr)"));
    auto* dial = menu.addAction(tr("Dial via relay hint"));
    QAction* chosen = menu.exec(row->mapToGlobal(pos));
    if (!chosen) return;
    if (chosen == copy_endpoint) {
        Q_EMIT copyEndpointRequested(hint.endpoint);
    } else if (chosen == dial) {
        Q_EMIT dialRelayHintRequested(hint);
    }
}

}  // namespace dinero::qt::dashboard
