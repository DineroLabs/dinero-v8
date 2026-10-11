#pragma once
#include <QWidget>

class QLabel;

// "Name: value" shown as a muted name on the left and the value right-aligned.
// setText/text keep the original combined string, so existing callers and
// exports that read text() are unchanged.
class InfoRow : public QWidget {
    Q_OBJECT
public:
    explicit InfoRow(const QString& text = QString(), QWidget* parent = nullptr);
    void setText(const QString& text);
    QString text() const { return raw_; }
    QString nameText() const;
    QString valueText() const;

private:
    QString raw_;
    QLabel* name_ = nullptr;
    QLabel* value_ = nullptr;
};

// Inserts thousands separators into whole numbers of 4+ digits; leaves
// decimals and already-grouped numbers alone ("121208" -> "121,208").
QString groupLongIntegers(const QString& text);
