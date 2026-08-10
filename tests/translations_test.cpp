// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "translations.h"
#include "tray_client.h"
#include "tray_window.h"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>

// The catalogues are built beside the binaries, and the suite is pointed at them
// through the same variable a person would use — see tests/CMakeLists.txt.
class TranslationsTest : public QObject {
    Q_OBJECT

private slots:
    void theCatalogueDirectoryFollowsTheVariable();
    void aWindowInAnotherLanguageSaysSoEverywhere();
    void anUnknownLanguageIsEnglish();
    void aMissingCatalogueIsNotAFailure();

private:
    // Rebuilt per case: a QTranslator installed by one of them would otherwise
    // decide what the next one sees.
    void withLanguage(const QString &language, const std::function<void()> &check);
};

void TranslationsTest::withLanguage(const QString &language,
                                    const std::function<void()> &check)
{
    // Named rather than set in the environment: Qt reads the system locale once,
    // at startup, so a variable changed here would change nothing. The console
    // takes the same argument and lets it default to the system's.
    translations::install(QCoreApplication::instance(), QLocale(language));
    check();
    translations::install(QCoreApplication::instance(), QLocale::c());
}

void TranslationsTest::theCatalogueDirectoryFollowsTheVariable()
{
    const QByteArray previous = qgetenv("OMBRA_TRANSLATIONS_DIR");
    qputenv("OMBRA_TRANSLATIONS_DIR", QByteArrayLiteral("/somewhere/else"));
    QCOMPARE(translations::directory(), QStringLiteral("/somewhere/else"));
    qputenv("OMBRA_TRANSLATIONS_DIR", previous);
    // Back to where the suite pointed it, which is the build tree.
    QVERIFY(!translations::directory().isEmpty());
}

void TranslationsTest::aWindowInAnotherLanguageSaysSoEverywhere()
{
    withLanguage(QStringLiteral("ru"), [] {
        TrayClient client(QStringLiteral("/nonexistent/ombra-translations-test.sock"));
        TrayWindow window(client);

        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("tabs"));
        QVERIFY(tabs);
        // Tab titles are translated; the names the pictures are called by are
        // not, because a document links to those.
        QCOMPARE(tabs->tabText(0), QString::fromUtf8("Статус"));
        QCOMPARE(tabs->widget(0)->objectName(), QStringLiteral("tab_status"));
        QCOMPARE(tabs->widget(3)->objectName(), QStringLiteral("tab_settings"));

        // A control, its tooltip, and the line the window says out loud: all
        // three come from the catalogue, and a translation that covered only the
        // captions would leave the explanations English.
        auto *appearance = window.findChild<QComboBox *>(QStringLiteral("appearance"));
        QCOMPARE(appearance->itemText(0), QString::fromUtf8("Как в системе"));
        QVERIFY(appearance->toolTip().contains(QString::fromUtf8("цвета")));
        QVERIFY(window.statusText().contains(
                QString::fromUtf8("Демон не отвечает")));

        // The vocabulary of the settings file stays as it is written there. A
        // translated `power` would name nothing the daemon accepts.
        auto *epp = window.findChild<QComboBox *>(QStringLiteral("epp"));
        QVERIFY(epp->findText(QStringLiteral("power")) >= 0);
    });
}

void TranslationsTest::anUnknownLanguageIsEnglish()
{
    // Not that the language is refused: English is the source language, so a
    // machine set to something with no catalogue shows the strings as the code
    // has them.
    withLanguage(QStringLiteral("fi"), [] {
        TrayClient client(QStringLiteral("/nonexistent/ombra-translations-test.sock"));
        TrayWindow window(client);
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("tabs"));
        QCOMPARE(tabs->tabText(0), QStringLiteral("Status"));
        QCOMPARE(tabs->tabText(3), QStringLiteral("Settings"));
    });
}

void TranslationsTest::aMissingCatalogueIsNotAFailure()
{
    QTemporaryDir empty;
    QVERIFY(empty.isValid());
    const QByteArray previous = qgetenv("OMBRA_TRANSLATIONS_DIR");
    qputenv("OMBRA_TRANSLATIONS_DIR", empty.path().toLocal8Bit());

    withLanguage(QStringLiteral("ru"), [] {
        TrayClient client(QStringLiteral("/nonexistent/ombra-translations-test.sock"));
        TrayWindow window(client);
        QCOMPARE(window.findChild<QTabWidget *>(QStringLiteral("tabs"))->tabText(0),
                 QStringLiteral("Status"));
    });

    qputenv("OMBRA_TRANSLATIONS_DIR", previous);
}

QTEST_MAIN(TranslationsTest)
#include "translations_test.moc"
