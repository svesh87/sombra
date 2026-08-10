// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "tray_icon.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPalette>
#include <QTemporaryDir>
#include <QTest>

namespace {

// What the panel actually receives. The icon is compared as pixels because
// that is the only thing a StatusNotifierItem host will draw once there is no
// theme name for it to prefer.
QImage rendered(const QIcon &icon)
{
    return icon.pixmap(QSize(22, 22)).toImage();
}

// A minimal icon theme with exactly the names given, so "the theme has this
// glyph" and "the theme does not" are both testable without depending on which
// themes the machine happens to have installed.
void writeTheme(const QString &root, const QString &name, const QStringList &icons)
{
    const QString directory = root + QLatin1Char('/') + name;
    QVERIFY(QDir().mkpath(directory + QStringLiteral("/scalable/status")));

    QFile index(directory + QStringLiteral("/index.theme"));
    QVERIFY(index.open(QIODevice::WriteOnly | QIODevice::Text));
    index.write("[Icon Theme]\nName=");
    index.write(name.toUtf8());
    index.write("\nDirectories=scalable/status\n\n[scalable/status]\nSize=16\n"
                "MinSize=8\nMaxSize=512\nContext=Status\nType=Scalable\n");
    index.close();

    for (const QString &icon : icons) {
        QFile file(directory + QStringLiteral("/scalable/status/") + icon
                   + QStringLiteral(".svg"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\">"
                   "<rect width=\"16\" height=\"16\" fill=\"#00ff00\"/></svg>");
    }
}

} // namespace

class TrayIconTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void theThemeIsAskedFirstAndItsNameIsWhatTravels();
    void aNameTheThemeDoesNotHaveIsNeverPublished();
    void everyModeLooksDifferent();
    void theTwoStatesThatAreNotModesLookDifferentAgain();
    void theColourSchemeChangesNothing();
    void anUnknownModeCountIsDrawnRatherThanGuessed();

private:
    QTemporaryDir m_themes;
};

void TrayIconTest::initTestCase()
{
    QVERIFY(m_themes.isValid());
    writeTheme(m_themes.path(), QStringLiteral("with-profiles"),
               { QStringLiteral("power-profile-power-saver-symbolic"),
                 QStringLiteral("power-profile-balanced-symbolic"),
                 QStringLiteral("power-profile-performance-symbolic"),
                 QStringLiteral("dialog-warning-symbolic") });
    writeTheme(m_themes.path(), QStringLiteral("without-profiles"),
               { QStringLiteral("folder-symbolic") });
    QIcon::setThemeSearchPaths({ m_themes.path() });
}

void TrayIconTest::theThemeIsAskedFirstAndItsNameIsWhatTravels()
{
    QIcon::setThemeName(QStringLiteral("with-profiles"));

    // The name is what a StatusNotifierItem host draws, and it has to be the
    // one that was asked for rather than whatever alias Qt resolved it to.
    QCOMPARE(trayIcon(true, false, 0, 3).name(),
             QStringLiteral("power-profile-power-saver-symbolic"));
    QCOMPARE(trayIcon(true, false, 1, 3).name(),
             QStringLiteral("power-profile-balanced-symbolic"));
    QCOMPARE(trayIcon(true, false, 2, 3).name(),
             QStringLiteral("power-profile-performance-symbolic"));
    QCOMPARE(trayIcon(true, true, 1, 3).name(), QStringLiteral("dialog-warning-symbolic"));

    // And the picture is the theme's, not ours: this theme draws flat green.
    const QImage image = rendered(trayIcon(true, false, 1, 3));
    QCOMPARE(image.pixelColor(11, 11), QColor(0, 255, 0));
}

void TrayIconTest::aNameTheThemeDoesNotHaveIsNeverPublished()
{
    // The failure this prevents: a host prefers the name over the picture sent
    // with it, so a name the panel's theme lacks is drawn as a question mark.
    QIcon::setThemeName(QStringLiteral("without-profiles"));

    for (const int index : { 0, 1, 2 }) {
        QCOMPARE(trayIcon(true, false, index, 3).name(), QString());
        QVERIFY(!rendered(trayIcon(true, false, index, 3)).isNull());
    }
    QCOMPARE(trayIcon(true, true, 1, 3).name(), QString());
    QCOMPARE(trayIcon(false, false, -1, 0).name(), QString());
}

void TrayIconTest::everyModeLooksDifferent()
{
    // Drawn by us, which is the case the rest of these check: a theme with the
    // names covers itself, and this is what happens on the machine this runs on.
    QIcon::setThemeName(QStringLiteral("without-profiles"));
    const QImage quiet = rendered(trayIcon(true, false, 0, 3));
    const QImage work = rendered(trayIcon(true, false, 1, 3));
    const QImage full = rendered(trayIcon(true, false, 2, 3));

    QVERIFY(!quiet.isNull());
    QVERIFY(quiet != work);
    QVERIFY(work != full);
    QVERIFY(quiet != full);

    // And the difference is a level rather than an arbitrary glyph: more power
    // is more of the fill, so more of the picture is painted at all.
    const auto painted = [](const QImage &image) {
        int count = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (image.pixelColor(x, y).alpha() > 128)
                    ++count;
            }
        }
        return count;
    };
    QVERIFY2(painted(quiet) < painted(work), "the cooler mode has to be the emptier one");
    QVERIFY(painted(work) < painted(full));
}

void TrayIconTest::theTwoStatesThatAreNotModesLookDifferentAgain()
{
    QIcon::setThemeName(QStringLiteral("without-profiles"));
    const QImage running = rendered(trayIcon(true, false, 1, 3));
    const QImage guard = rendered(trayIcon(true, true, 1, 3));
    const QImage absent = rendered(trayIcon(false, false, -1, 0));

    QVERIFY(running != guard);
    QVERIFY(running != absent);
    QVERIFY(guard != absent);
}

void TrayIconTest::theColourSchemeChangesNothing()
{
    // The reason the colour is not taken from the palette: a desktop can be
    // light with a dark bar, and then the application's palette is the wrong
    // authority on what the panel needs.
    QIcon::setThemeName(QStringLiteral("without-profiles"));
    const QPalette original = QApplication::palette();

    QPalette light;
    light.setColor(QPalette::Window, Qt::white);
    light.setColor(QPalette::WindowText, Qt::black);
    QApplication::setPalette(light);
    const QImage onLight = rendered(trayIcon(true, false, 1, 3));

    QPalette dark;
    dark.setColor(QPalette::Window, Qt::black);
    dark.setColor(QPalette::WindowText, Qt::white);
    QApplication::setPalette(dark);
    const QImage onDark = rendered(trayIcon(true, false, 1, 3));

    QApplication::setPalette(original);
    QCOMPARE(onLight, onDark);
}

void TrayIconTest::anUnknownModeCountIsDrawnRatherThanGuessed()
{
    QIcon::setThemeName(QStringLiteral("without-profiles"));
    // Before the first status reply there is no table and no position in it.
    const QImage unknown = rendered(trayIcon(true, false, -1, 0));
    QVERIFY(!unknown.isNull());
    QVERIFY(unknown != rendered(trayIcon(true, false, 0, 3)));

    // A table of another size still reads as a level, and the last mode fills
    // it whatever the count.
    QCOMPARE(rendered(trayIcon(true, false, 3, 4)), rendered(trayIcon(true, false, 2, 3)));
    // An index past the end is the last mode rather than an overflowing fill.
    QCOMPARE(rendered(trayIcon(true, false, 9, 3)), rendered(trayIcon(true, false, 2, 3)));
}

QTEST_MAIN(TrayIconTest)
#include "tray_icon_test.moc"
