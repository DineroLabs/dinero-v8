#pragma once
#include <QVector>
#include <QWidget>

class QLabel;
class QHBoxLayout;

// "Name: value" shown as a muted name on the left and the value right-aligned.
// setText/text keep the original combined string, so existing callers and
// exports that read text() are unchanged.
class InfoRow : public QWidget {
    Q_OBJECT
public:
    enum class ValueStyle { Plain, Scoreboard, SmallScoreboard };
    explicit InfoRow(const QString& text = QString(), QWidget* parent = nullptr);
    void setText(const QString& text);
    void setValueStyle(ValueStyle style);
    QString text() const { return raw_; }
    QString nameText() const;
    QString valueText() const;

private:
    void updateDigitalValue(const QString& formatted);
    QString raw_;
    QLabel* name_ = nullptr;
    QLabel* value_ = nullptr;
    QWidget* valueContainer_ = nullptr;
    QHBoxLayout* valueLayout_ = nullptr;
    QVector<QLabel*> valueParts_;
    QVector<bool> numberParts_;
    ValueStyle valueStyle_ = ValueStyle::Plain;
};

// Inserts thousands separators into whole numbers of 4+ digits; leaves
// decimals and already-grouped numbers alone ("121208" -> "121,208").
QString groupLongIntegers(const QString& text);
