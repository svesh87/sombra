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

// The light that gets through: warm pixels, which the sun and its rays are and
// neither the blue plate nor the dark slats can be. Not usable while the guard
// is clamping, because the plate is warm then too.
int sunlight(const QImage &image)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.red() > pixel.blue() + 60)
                ++count;
        }
    }
    return count;
}

// The persiana itself: opaque and dark on every channel, which nothing else in
// the picture is. This is the measure that also works under the guard's red
// plate. Transparency is checked first, because the corners outside the plate
// are transparent black and would otherwise count as slat.
int slatPixels(const QImage &image)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() > 128 && pixel.red() < 80 && pixel.green() < 80
                && pixel.blue() < 90)
                ++count;
        }
    }
    return count;
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

    // And the difference is a position in the table rather than an arbitrary
    // glyph: the cooler the envelope, the further the persiana is shut, so less
    // light gets through and more of the picture is slat.
    QVERIFY2(sunlight(quiet) < sunlight(work), "the cooler mode has to be the darker one");
    QVERIFY(sunlight(work) < sunlight(full));
    QVERIFY(slatPixels(quiet) > slatPixels(work));
    QVERIFY(slatPixels(work) > slatPixels(full));
}

void TrayIconTest::theTwoStatesThatAreNotModesLookDifferentAgain()
{
    const QImage running = rendered(trayIcon(true, false, 1, 3));
    const QImage guard = rendered(trayIcon(true, true, 1, 3));
    const QImage absent = rendered(trayIcon(false, false, -1, 0));

    QVERIFY(running != guard);
    QVERIFY(running != absent);
    QVERIFY(guard != absent);

    // Nothing is answering, so there is no mode to show and the persiana is
    // gone. The picture says the sun is unattended rather than shut out.
    QCOMPARE(slatPixels(absent), 0);
    QVERIFY(slatPixels(running) > 0);

    // The channels are independent: the guard changes the plate and leaves the
    // mode where it was, so a clamped machine still says which envelope it is in.
    QCOMPARE(slatPixels(guard), slatPixels(running));
    QVERIFY(slatPixels(trayIcon(true, true, 0, 3).pixmap(22, 22).toImage())
            > slatPixels(guard));
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
    // Before the first status reply there is no table and no position in it, so
    // there is no persiana either — but the sun is warm, which is what separates
    // "answering, mode not known yet" from "nothing is answering".
    const QImage unknown = rendered(trayIcon(true, false, -1, 0));
    QVERIFY(!unknown.isNull());
    QVERIFY(unknown != rendered(trayIcon(true, false, 0, 3)));
    QCOMPARE(slatPixels(unknown), 0);
    QVERIFY(unknown != rendered(trayIcon(false, false, -1, 0)));
    QVERIFY(sunlight(unknown) > sunlight(rendered(trayIcon(false, false, -1, 0))));

    // A table of another size still reads as a position in it, and the last mode
    // opens the persiana fully whatever the count.
    QCOMPARE(rendered(trayIcon(true, false, 3, 4)), rendered(trayIcon(true, false, 2, 3)));
    // An index past the end is the last mode rather than an overflowing fill.
    QCOMPARE(rendered(trayIcon(true, false, 9, 3)), rendered(trayIcon(true, false, 2, 3)));
}

QTEST_MAIN(TrayIconTest)
#include "tray_icon_test.moc"
