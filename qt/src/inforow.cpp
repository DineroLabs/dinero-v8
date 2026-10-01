#include "inforow.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QRegularExpression>

QString groupLongIntegers(const QString& text) {
    // A run of 4+ digits not touching '.', ',' or another digit.
    static const QRegularExpression run(QStringLiteral("(?<![\\d.,])(\\d{4,})(?![\\d.,]\\d)"));
    QString out;
    qsizetype last = 0;
    auto it = run.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        out += text.mid(last, m.capturedStart(1) - last);
        QString digits = m.captured(1);
        for (qsizetype i = digits.size() - 3; i > 0; i -= 3) digits.insert(i, ',');
        out += digits;
        last = m.capturedEnd(1);
    }
    out += text.mid(last);
    return out;
}

InfoRow::InfoRow(const QString& text, QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 1, 0, 1);
    name_ = new QLabel(this);
    value_ = new QLabel(this);
    name_->setStyleSheet("QLabel { color: #8f9aa6; background: transparent; }");
    value_->setStyleSheet("QLabel { color: #d6dde6; background: transparent; }");
    value_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    value_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(name_);
    layout->addStretch(1);
    layout->addWidget(value_);
    setText(text);
}

void InfoRow::setText(const QString& text) {
    raw_ = text;
    const qsizetype colon = text.indexOf(QStringLiteral(": "));
    if (colon > 0) {
        name_->setText(text.left(colon));
        value_->setText(groupLongIntegers(text.mid(colon + 2)));
    } else {
        name_->clear();
        value_->setText(groupLongIntegers(text));
    }
}

QString InfoRow::nameText() const { return name_->text(); }
QString InfoRow::valueText() const { return value_->text(); }
