#pragma once

#include <QString>

QString normalizeWalletName(const QString& rawName);
QString validateWalletNameInput(const QString& rawName, QString* normalizedName = nullptr);
