// Gates the interface-translation chain for dinero-qt.
//
// It proves the properties the plan depends on, not merely that a file loads:
//   1. A finished translation is applied.
//   2. A marked-but-UNtranslated string falls back to its English source.
//      This is the rule that lets a language ship before it is complete.
//   3. A string that was never marked is returned unchanged.
//   4. The catalog resource path and settings key the app uses are stable.
//
// Checks exit non-zero rather than using assert(), which is a no-op under
// NDEBUG and would silently pass in a Release build.

#include "../src/i18n.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QString>
#include <QTranslator>

#include <cstdio>

namespace {

int g_failures = 0;

void Check(const char* what, bool ok, const QString& detail = QString()) {
  std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
              detail.isEmpty() ? "" : " -- ",
              detail.isEmpty() ? "" : detail.toUtf8().constData());
  if (!ok) ++g_failures;
}

void CheckEqual(const char* what, const QString& actual, const QString& expected) {
  const bool ok = (actual == expected);
  Check(what, ok,
        ok ? QString()
           : QStringLiteral("expected \"%1\", got \"%2\"").arg(expected, actual));
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);

  if (argc < 2) {
    std::fprintf(stderr, "usage: test_i18n <path to dinero_de.qm>\n");
    return 2;
  }
  const QString qm_path = QString::fromLocal8Bit(argv[1]);
  if (!QFileInfo::exists(qm_path)) {
    std::fprintf(stderr, "catalog not found: %s\n", qm_path.toUtf8().constData());
    return 2;
  }

  std::printf("== dinero-qt translation chain ==\ncatalog: %s\n",
              qm_path.toUtf8().constData());

  // Settings contract: these two strings are what main.cpp and the Settings
  // picker rely on. Changing them silently would orphan every stored choice.
  std::printf("\n-- contract --\n");
  CheckEqual("settings key is ui/language",
             dinero::qt::i18n::LanguageSettingsKey(), QStringLiteral("ui/language"));
  CheckEqual("source language is English",
             dinero::qt::i18n::SourceLanguageCode(), QStringLiteral("en"));
  CheckEqual("catalog resource path",
             dinero::qt::i18n::CatalogResourcePath(QStringLiteral("de")),
             QStringLiteral(":/i18n/dinero_de.qm"));

  // Before the translator is installed, every string is its English self.
  std::printf("\n-- before install --\n");
  CheckEqual("English source returned when no catalog is installed",
             QCoreApplication::translate("MainWindow", "Settings"),
             QStringLiteral("Settings"));

  QTranslator translator;
  Check("catalog loads", translator.load(qm_path));
  Check("translator installs", QCoreApplication::installTranslator(&translator));

  std::printf("\n-- after install --\n");
  // 1. Finished translations are applied.
  CheckEqual("finished translation is applied (Settings)",
             QCoreApplication::translate("MainWindow", "Settings"),
             QString::fromUtf8("Einstellungen"));
  CheckEqual("finished translation is applied (Show Developer Menu)",
             QCoreApplication::translate("MainWindow", "Show Developer Menu"),
             QString::fromUtf8("Entwicklermen\xC3\xBC anzeigen"));
  CheckEqual("finished translation is applied (Interface language:)",
             QCoreApplication::translate("MainWindow", "Interface language:"),
             QString::fromUtf8("Sprache der Benutzeroberfl\xC3\xA4"
                               "che:"));

  // 2. THE FALLBACK RULE. This string is marked with tr() in mainwindow.cpp and
  //    appears in the .ts, but is deliberately left untranslated, so lrelease
  //    omits it from the catalog and English must come back.
  CheckEqual("marked-but-untranslated string falls back to English",
             QCoreApplication::translate(
                 "MainWindow",
                 "Use these controls when testing daemon startup, connection "
                 "recovery, or local runtime health."),
             QStringLiteral("Use these controls when testing daemon startup, "
                            "connection recovery, or local runtime health."));

  // 3. A string that was never marked is untouched.
  CheckEqual("never-marked string is returned unchanged",
             QCoreApplication::translate("MainWindow",
                                         "this string is not in any catalog"),
             QStringLiteral("this string is not in any catalog"));

  std::printf("\n== %s ==\n", g_failures == 0 ? "ALL CHECKS PASSED"
                                              : "FAILURES PRESENT");
  return g_failures == 0 ? 0 : 1;
}
