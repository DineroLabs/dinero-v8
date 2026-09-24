// Interface language support for dinero-qt.
//
// Design rules this module implements (see qt/MULTI-LANGUAGE-PLAN.md):
//   1. English is the SOURCE language. There is no English catalog: with no
//      translator installed, tr() returns the English source text. A string
//      missing from a catalog therefore falls back to English automatically.
//   2. The language is chosen only in Settings and applied on restart. Nothing
//      here retranslates a live widget tree.
//   3. WORDS ONLY. This module never touches QLocale::setDefault(), so number
//      and amount formatting stays on the C locale (dot decimal separator) in
//      every language. Changing that would alter how amounts are parsed and
//      displayed, which is a fund-loss risk, not a cosmetic one.
//   4. No operating-system locale auto-detection. An unset preference means
//      English, not the machine's locale.
//
// Catalogs are compiled to .qm by lrelease and embedded in the binary under
// :/i18n/, so a shipped .app needs no external translation files.

#pragma once

#include <QString>
#include <QVector>

class QCoreApplication;

namespace dinero::qt::i18n {

// A language offered in the Settings picker.
struct Language {
  QString code;        // Qt locale code, e.g. "de", "pt_BR", "zh_CN"
  QString nativeName;  // shown in the picker, in that language
};

// QSettings key holding the chosen language code.
QString LanguageSettingsKey();

// English source language code.
QString SourceLanguageCode();

// Languages the running binary can actually display: English, plus every
// language whose compiled catalog is embedded in this build. A language with
// no catalog is deliberately NOT offered, because selecting it would show a
// fully English interface and read as broken.
QVector<Language> AvailableLanguages();

// Chosen language code, or English when unset or when the stored value names a
// language this build has no catalog for.
QString CurrentLanguageCode();

// Persist the chosen language. Takes effect on the next start by design.
void SetLanguageCode(const QString& code);

// Resource path of a language's compiled catalog.
QString CatalogResourcePath(const QString& code);

// Install the translator for the stored language. Must be called after
// QCoreApplication::setOrganizationName() so QSettings resolves, and before any
// widget is constructed. Returns true when a catalog was installed; English
// returns false because it needs none, which is not an error.
bool InstallTranslator(QCoreApplication& app);

}  // namespace dinero::qt::i18n
