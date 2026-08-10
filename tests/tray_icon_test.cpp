// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "tray_icon.h"

#include <QApplication>
#include <QImage>
#include <QPalette>
#include <QTest>

namespace {

// What the panel actually receives. The icon is compared as pixels because
// that is the only thing a StatusNotifierItem host will draw once there is no
// theme name for it to prefer.
QImage rendered(const QIcon &icon)
{
    return icon.pixmap(QSize(22, 22)).toImage();
}

} // namespace

class TrayIconTest : public QObject {
    Q_OBJECT

private slots:
    void nothingIsLeftForAPanelToSubstitute();
    void everyModeLooksDifferent();
    void theTwoStatesThatAreNotModesLookDifferentAgain();
    void theColourSchemeChangesNothing();
    void anUnknownModeCountIsDrawnRatherThanGuessed();

};

void TrayIconTest::nothingIsLeftForAPanelToSubstitute()
{
    // No theme name travels with the icon, whatever the icon theme in force.
    // A StatusNotifierItem host prefers a name over the picture sent with it and
    // draws a placeholder for one it cannot resolve — and it resolves names in
    // its own theme, which is not the theme this process can ask.
    for (const QString &theme : { QStringLiteral("Adwaita"), QStringLiteral("does-not-exist") }) {
        QIcon::setThemeName(theme);
        QCOMPARE(trayIcon(true, false, 0, 3).name(), QString());
        QCOMPARE(trayIcon(true, false, 1, 3).name(), QString());
        QCOMPARE(trayIcon(true, true, 1, 3).name(), QString());
        QCOMPARE(trayIcon(false, false, -1, 0).name(), QString());
        QVERIFY(!rendered(trayIcon(true, false, 1, 3)).isNull());
    }
}

void TrayIconTest::everyModeLooksDifferent()
{
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
