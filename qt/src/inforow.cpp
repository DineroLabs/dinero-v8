#include "inforow.h"
#include "scoreboardlabel.h"
#include <QHBoxLayout>
#include <QFont>
#include <QLabel>
#include <QRegularExpression>
#include <QStringList>

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
    // Names in capitals (display only: nameText() keeps the original text)
    // in the same whitish family as the value, one step softer.
    QFont nameFont = name_->font();
    nameFont.setCapitalization(QFont::AllUppercase);
    nameFont.setPointSizeF(nameFont.pointSizeF() * 0.8);  // capitals read 20% smaller
    nameFont.setLetterSpacing(QFont::PercentageSpacing, 104);
    name_->setFont(nameFont);
    name_->setStyleSheet("QLabel { color: #b9c2cc; background: transparent; }");
    value_->setStyleSheet("QLabel { color: #ccd3dc; background: transparent; }");
    value_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    value_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    valueContainer_ = new QWidget(this);
    valueLayout_ = new QHBoxLayout(valueContainer_);
    valueLayout_->setContentsMargins(0, 0, 0, 0);
    valueLayout_->setSpacing(0);
    valueContainer_->hide();
    layout->addWidget(name_);
    layout->addStretch(1);
    layout->addWidget(value_);
    layout->addWidget(valueContainer_);
    setText(text);
}

void InfoRow::setValueStyle(ValueStyle style) {
    if (valueStyle_ == style) return;
    valueStyle_ = style;
    value_->setVisible(style == ValueStyle::Plain);
    valueContainer_->setVisible(style != ValueStyle::Plain);
    if (style != ValueStyle::Plain) updateDigitalValue(value_->text());
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
    if (valueStyle_ != ValueStyle::Plain) updateDigitalValue(value_->text());
}

void InfoRow::updateDigitalValue(const QString& formatted) {
    // Keep the full formatted string in value_ for text(), accessibility, and
    // callers; reuse the visible child labels as live values change.
    static const QRegularExpression numbers(QStringLiteral("\\d[\\d,.]*"));
    QStringList parts;
    QVector<bool> numberParts;
    qsizetype offset = 0;
    auto matches = numbers.globalMatch(formatted);
    while (matches.hasNext()) {
        const auto match = matches.next();
        if (match.capturedStart() > offset) {
            parts.append(formatted.mid(offset, match.capturedStart() - offset));
            numberParts.append(false);
        }
        parts.append(match.captured());
        numberParts.append(true);
        offset = match.capturedEnd();
    }
    if (offset < formatted.size() || parts.isEmpty()) {
        parts.append(formatted.mid(offset));
        numberParts.append(false);
    }
    if (numberParts_ != numberParts) {
        while (auto* item = valueLayout_->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        valueParts_.clear();
        numberParts_ = numberParts;
        for (bool isNumber : numberParts_) {
            QLabel* part = isNumber
                ? static_cast<QLabel*>(new MiningScoreboardLabel(QString(), valueContainer_))
                : new QLabel(valueContainer_);
            part->setStyleSheet("QLabel { color: #ccd3dc; background: transparent; }");
            part->setAlignment(Qt::AlignVCenter);
            if (isNumber) {
                auto* scoreboard = static_cast<MiningScoreboardLabel*>(part);
                scoreboard->setGlyphScale(
                    valueStyle_ == ValueStyle::Scoreboard ? 1.0 : 0.75);
                scoreboard->setFitToWidth(true);
            }
            valueLayout_->addWidget(part);
            valueParts_.append(part);
        }
    }
    for (qsizetype i = 0; i < parts.size(); ++i) {
        if (numberParts_[i]) {
            static_cast<MiningScoreboardLabel*>(valueParts_[i])->setGlyphScale(
                valueStyle_ == ValueStyle::Scoreboard ? 1.0 : 0.75);
        }
        valueParts_[i]->setText(parts[i]);
    }
    valueContainer_->setAccessibleName(formatted);
}

QString InfoRow::nameText() const { return name_->text(); }
QString InfoRow::valueText() const { return value_->text(); }
