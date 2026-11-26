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
#include "../src/apprestart.h"

#include <QCoreApplication>
#include <QFile>
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

  if (argc < 3) {
    std::fprintf(stderr, "usage: test_i18n <dinero_de.qm> <dinero_sl.qm>\n");
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
  bool allFlagsEmbedded = true;
  for (const char* country : {"gb", "es", "cn", "ru", "br", "de", "fr",
                              "tr", "pl", "ua", "jp", "vn", "ba", "se", "si"}) {
    allFlagsEmbedded &= QFile::exists(
        QStringLiteral(":/language-flags/%1.png").arg(QString::fromLatin1(country)));
  }
  Check("flags for every offered language are embedded", allFlagsEmbedded);
  Check("flag artwork license is embedded",
        QFile::exists(QStringLiteral(":/language-flags/LICENSE.flag-icons")));
  bool sloveneAvailable = false;
  bool allAvailableFlagsExist = true;
  for (const auto& language : dinero::qt::i18n::AvailableLanguages()) {
    sloveneAvailable |= language.code == QStringLiteral("sl");
    allAvailableFlagsExist &= QFile::exists(language.flagIconPath);
  }
  Check("Slovenian appears in the language picker", sloveneAvailable);
  Check("every offered language has its flag", allAvailableFlagsExist);

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

  // A string that was never marked is untouched.
  CheckEqual("never-marked string is returned unchanged",
             QCoreApplication::translate("MainWindow",
                                         "this string is not in any catalog"),
             QStringLiteral("this string is not in any catalog"));

  QCoreApplication::removeTranslator(&translator);

  {
    const QString slovenePath = QString::fromLocal8Bit(argv[2]);
    QTranslator slovene;
    Check("Slovenian catalog loads", slovene.load(slovenePath));
    Check("Slovenian translator installs", QCoreApplication::installTranslator(&slovene));
    CheckEqual("Slovenian Settings label",
               QCoreApplication::translate("MainWindow", "Settings"),
               QString::fromUtf8("Nastavitve"));
    CheckEqual("Slovenian wallet setup label",
               QCoreApplication::translate("WelcomePage", "Welcome to Dinero Wallet"),
               QString::fromUtf8("Dobrodošli v denarnici Dinero"));
    CheckEqual("Slovenian label preserves the H/s unit",
               QCoreApplication::translate("MainWindow", "Network: 0 H/s"),
               QString::fromUtf8("Omrežje: 0 H/s"));
    // Template is marked with tr() but intentionally has no Slovenian entry.
    // The English fallback remains visible without changing protocol terms.
    CheckEqual("marked-but-untranslated text falls back to English",
               QCoreApplication::translate("MainWindow", "Template"),
               QStringLiteral("Template"));
    QCoreApplication::removeTranslator(&slovene);
  }

  // ---- restart contract -----------------------------------------------
  // The language picker offers an in-place restart. These properties are what
  // keep it from colliding with the outgoing instance: the replacement must
  // carry the wait flag exactly once and must keep the SAME datadir, or it
  // restarts onto a different wallet or bounces off the single-instance lock.
  std::printf("\n-- restart contract --\n");
  {
    namespace restart = dinero::qt::apprestart;
    const QStringList original{QStringLiteral("/Applications/dinero-qt"),
                               QStringLiteral("-datadir=/tmp/wallet a"),
                               QStringLiteral("--some-flag")};
    const QStringList relaunch = restart::RelaunchArguments(original);

    Check("program path is dropped from the relaunch arguments",
          !relaunch.contains(QStringLiteral("/Applications/dinero-qt")));
    Check("datadir is preserved",
          relaunch.contains(QStringLiteral("-datadir=/tmp/wallet a")));
    Check("other arguments are preserved",
          relaunch.contains(QStringLiteral("--some-flag")));
    CheckEqual("wait flag present exactly once",
               QString::number(relaunch.count(restart::AwaitingRestartFlag())),
               QStringLiteral("1"));
    Check("replacement is detected as awaiting restart",
          restart::StartedAwaitingRestart(relaunch));

    // Restarting twice must not accumulate the flag.
    QStringList second = restart::RelaunchArguments(
        QStringList{QStringLiteral("/Applications/dinero-qt")} + relaunch);
    CheckEqual("flag still appears once after a second restart",
               QString::number(second.count(restart::AwaitingRestartFlag())),
               QStringLiteral("1"));
    Check("a normal start is NOT treated as awaiting restart",
          !restart::StartedAwaitingRestart(
              QStringList{QStringLiteral("/Applications/dinero-qt"),
                          QStringLiteral("-datadir=/tmp/x")}));
    Check("lock wait is long enough for a daemon stop",
          restart::LockWaitMilliseconds() >= 10000);
  }

  std::printf("\n== %s ==\n", g_failures == 0 ? "ALL CHECKS PASSED"
                                              : "FAILURES PRESENT");
  return g_failures == 0 ? 0 : 1;
}
