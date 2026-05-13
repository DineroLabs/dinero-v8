#pragma once

class QAbstractScrollArea;
class QScrollArea;
class QWidget;

namespace ScrollSupport {

void enableForScrollArea(QAbstractScrollArea* scrollArea, QWidget* proxyRoot = nullptr);
QScrollArea* wrapInScrollArea(QWidget* page, QWidget* parent = nullptr);

}  // namespace ScrollSupport
