#include "scrollsupport.h"

#include <QAbstractScrollArea>
#include <QEvent>
#include <QLayout>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QScroller>
#include <QWheelEvent>
#include <QWidget>
#include <algorithm>

namespace {

int verticalWheelDelta(const QWheelEvent* wheel)
{
    if (!wheel) {
        return 0;
    }
    if (!wheel->pixelDelta().isNull()) {
        return wheel->pixelDelta().y();
    }
    return wheel->angleDelta().y() / 8;
}

QAbstractScrollArea* owningScrollArea(QObject* watched)
{
    auto* widget = qobject_cast<QWidget*>(watched);
    while (widget) {
        if (auto* scrollArea = qobject_cast<QAbstractScrollArea*>(widget)) {
            return scrollArea;
        }
        widget = widget->parentWidget();
    }
    return nullptr;
}

bool scrollAreaCanConsume(const QAbstractScrollArea* scrollArea, int delta)
{
    if (!scrollArea || delta == 0) {
        return false;
    }

    const auto* bar = scrollArea->verticalScrollBar();
    if (!bar || bar->maximum() <= bar->minimum()) {
        return false;
    }

    if (delta > 0) {
        return bar->value() > bar->minimum();
    }
    return bar->value() < bar->maximum();
}

class ScrollProxyFilter : public QObject {
public:
    explicit ScrollProxyFilter(QAbstractScrollArea* target, QObject* parent = nullptr)
        : QObject(parent)
        , target_(target)
    {
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!target_ || event->type() != QEvent::Wheel) {
            return QObject::eventFilter(watched, event);
        }

        auto* wheel = static_cast<QWheelEvent*>(event);
        const int delta = verticalWheelDelta(wheel);
        if (delta == 0) {
            return QObject::eventFilter(watched, event);
        }

        auto* targetBar = target_->verticalScrollBar();
        if (!targetBar || targetBar->maximum() <= targetBar->minimum()) {
            return QObject::eventFilter(watched, event);
        }

        if (auto* sourceArea = owningScrollArea(watched)) {
            if (sourceArea != target_ && scrollAreaCanConsume(sourceArea, delta)) {
                return QObject::eventFilter(watched, event);
            }
        }

        const int nextValue = std::clamp(targetBar->value() - delta,
                                         targetBar->minimum(),
                                         targetBar->maximum());
        if (nextValue == targetBar->value()) {
            return QObject::eventFilter(watched, event);
        }

        targetBar->setValue(nextValue);
        event->accept();
        return true;
    }

private:
    QPointer<QAbstractScrollArea> target_;
};

void installProxyRecursive(QWidget* root, QObject* filter)
{
    if (!root || !filter) {
        return;
    }

    root->installEventFilter(filter);
    const auto children = root->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget* child : children) {
        installProxyRecursive(child, filter);
    }
}

}  // namespace

namespace ScrollSupport {

void enableForScrollArea(QAbstractScrollArea* scrollArea, QWidget* proxyRoot)
{
    if (!scrollArea) {
        return;
    }

    if (scrollArea->focusPolicy() == Qt::NoFocus) {
        scrollArea->setFocusPolicy(Qt::StrongFocus);
    }

    if (scrollArea->viewport()) {
        scrollArea->viewport()->setAttribute(Qt::WA_AcceptTouchEvents, true);
        QScroller::grabGesture(scrollArea->viewport(), QScroller::TouchGesture);
    }

    auto* filter = new ScrollProxyFilter(scrollArea, scrollArea);
    scrollArea->installEventFilter(filter);
    if (scrollArea->viewport()) {
        scrollArea->viewport()->installEventFilter(filter);
    }
    installProxyRecursive(proxyRoot ? proxyRoot : scrollArea, filter);
}

QScrollArea* wrapInScrollArea(QWidget* page, QWidget* parent)
{
    if (page && page->layout()) {
        page->layout()->setSizeConstraint(QLayout::SetMinAndMaxSize);
    }

    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setAlignment(Qt::AlignTop);
    scroll->setWidget(page);
    enableForScrollArea(scroll, page);
    return scroll;
}

}  // namespace ScrollSupport
