#include "i18n.h"

#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <QTranslator>

namespace dinero::qt::i18n {
namespace {

// Languages the project intends to ship, in the agreed wave order. An entry
// here is only OFFERED once its compiled catalog is embedded in the build, so
// this table can list a language before its translation exists.
//
// Native names are written in their own language on purpose: a user looking for
// their language should not have to read English to find it.
struct Candidate {
  const char* code;
  const char* nativeName;
};

constexpr Candidate kCandidates[] = {
    // Wave 1
    {"es", "Espa\xC3\xB1ol"},
    {"zh_CN", "\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87"},
    {"ru", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9"},
    {"pt_BR", "Portugu\xC3\xAAs (Brasil)"},
    {"de", "Deutsch"},
    // Wave 2
    {"fr", "Fran\xC3\xA7\x61is"},
    {"tr", "T\xC3\xBCrk\xC3\xA7\x65"},
    {"pl", "Polski"},
    {"uk", "\xD0\xA3\xD0\xBA\xD1\x80\xD0\xB0\xD1\x97\xD0\xBD\xD1\x81\xD1\x8C\xD0\xBA\xD0\xB0"},
    {"ja", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"},
    {"vi", "Ti\xE1\xBA\xBFng Vi\xE1\xBB\x87t"},
    // Wave 3
    {"bs", "Bosanski"},
};

bool CatalogExists(const QString& code) {
  return QFile::exists(CatalogResourcePath(code));
}

}  // namespace

QString LanguageSettingsKey() { return QStringLiteral("ui/language"); }

QString SourceLanguageCode() { return QStringLiteral("en"); }

QString CatalogResourcePath(const QString& code) {
  return QStringLiteral(":/i18n/dinero_%1.qm").arg(code);
}

QVector<Language> AvailableLanguages() {
  QVector<Language> languages;
  // English is always offered and needs no catalog: it is the source language.
  languages.push_back(Language{SourceLanguageCode(), QStringLiteral("English")});
  for (const Candidate& candidate : kCandidates) {
    const QString code = QString::fromUtf8(candidate.code);
    if (CatalogExists(code)) {
      languages.push_back(Language{code, QString::fromUtf8(candidate.nativeName)});
    }
  }
  return languages;
}

QString CurrentLanguageCode() {
  const QString stored = QSettings().value(LanguageSettingsKey()).toString().trimmed();
  if (stored.isEmpty() || stored == SourceLanguageCode()) {
    return SourceLanguageCode();
  }
  // A stored language whose catalog is absent (downgrade, or a catalog dropped
  // from a later build) falls back to English rather than showing a half-
  // translated interface from a stale selection.
  return CatalogExists(stored) ? stored : SourceLanguageCode();
}

void SetLanguageCode(const QString& code) {
  QSettings().setValue(LanguageSettingsKey(), code.trimmed());
}

bool InstallTranslator(QCoreApplication& app) {
  const QString code = CurrentLanguageCode();
  if (code == SourceLanguageCode()) {
    return false;  // English is the source text; no catalog needed.
  }
  // Owned by the application object so it outlives this call and is destroyed
  // with the app rather than leaked.
  auto* translator = new QTranslator(&app);
  if (!translator->load(CatalogResourcePath(code))) {
    delete translator;
    return false;
  }
  return QCoreApplication::installTranslator(translator);
}

}  // namespace dinero::qt::i18n
