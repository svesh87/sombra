// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "tray_icon.h"

#include <QApplication>
#include <QImage>
#include <QPalette>
#include <QTest>

#include <algorithm>

namespace {

// What the panel actually receives. The icon is compared as pixels because
// that is the only thing a StatusNotifierItem host will draw once there is no
// theme name for it to prefer.
QImage rendered(const QIcon &icon)
{
    return icon.pixmap(QSize(22, 22)).toImage();
}

// The light that gets through: warm pixels, which the sun and its rays are and
// neither the blue plate nor the cloud can be. Not usable while the guard is
// clamping, because the plate is warm then too.
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

// The cloud itself: bright and neutral on every channel, which nothing else in
// the picture is — the plate is dark in both of its colours, the sun is warm, and
// the grey the cloud takes when the mode is unknown is still darker than this
// floor. This is the measure that also works under the guard's red plate.
// Transparency is checked first, because the corners outside the plate are
// transparent black and would otherwise count as nothing at all.
int cloudPixels(const QImage &image)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() <= 128)
                continue;
            const int low = std::min({ pixel.red(), pixel.green(), pixel.blue() });
            const int high = std::max({ pixel.red(), pixel.green(), pixel.blue() });
            if (low > 180 && high - low < 40)
                ++count;
        }
    }
    return count;
}

// Where the cloud is, as opposed to how much of it there is. The rim is blended
// with whatever lies under it, so a picture drawn over another plate colour counts
// a pixel or two differently; the seat is what has to hold still.
QRect cloudBounds(const QImage &image)
{
    QRect bounds;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() <= 128)
                continue;
            const int low = std::min({ pixel.red(), pixel.green(), pixel.blue() });
            const int high = std::max({ pixel.red(), pixel.green(), pixel.blue() });
            if (low > 180 && high - low < 40)
                bounds |= QRect(x, y, 1, 1);
        }
    }
    return bounds;
}

} // namespace

class TrayIconTest : public QObject {
    Q_OBJECT

private slots:
    void nothingIsLeftForAPanelToSubstitute();
    void everyModeLooksDifferent();
    void theTwoStatesThatAreNotModesLookDifferentAgain();
    void theColourSchemeChangesNothing();
    void nothingKnownAboutTheModeIsOnePicture();
    void aTableOfAnySizeReadsAsAPositionInIt();
    void theWindowGetsAnIconOfItsOwn();
};

void TrayIconTest::theWindowGetsAnIconOfItsOwn()
{
    // The window's icon is the installed file, and the suite is pointed at the source
    // tree for the same reason it is pointed there for the help pages: a lookup in the
    // icon theme would answer with the last release rather than with this build.
    const QByteArray directory = qgetenv("SOMBRA_ICONS_DIR");
    QVERIFY2(!directory.isEmpty(), "the suite has to say where the pictures are");

    const QIcon icon = applicationIcon();
    QVERIFY(!icon.isNull());
    // Drawn, not merely found: an icon that resolves to nothing still answers a
    // request for a pixmap, with an empty one.
    const QImage image = icon.pixmap(QSize(64, 64)).toImage();
    QVERIFY(!image.isNull());
    QVERIFY(sunlight(image) > 0);

    // The override goes in front of the theme rather than behind it, and asking twice
    // does not stack it up.
    const QStringList paths = QIcon::fallbackSearchPaths();
    QCOMPARE(paths.first(), QString::fromLocal8Bit(directory));
    applicationIcon();
    QCOMPARE(QIcon::fallbackSearchPaths(), paths);
}

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
    // glyph: the cooler the envelope, the more of the sun the cloud has taken.
    // In the coolest one the sun is not drawn at all, so no light gets through.
    QCOMPARE(sunlight(quiet), 0);
    QVERIFY2(sunlight(work) < sunlight(full), "the cooler mode has to be the darker one");
    QVERIFY(sunlight(work) > 0);

    // The cloud is where the mode is: over the whole picture, over the sun, gone.
    QVERIFY(cloudPixels(quiet) > 0);
    QVERIFY(cloudPixels(work) > 0);
    QCOMPARE(cloudPixels(full), 0);
}

void TrayIconTest::theTwoStatesThatAreNotModesLookDifferentAgain()
{
    const QImage running = rendered(trayIcon(true, false, 1, 3));
    const QImage guard = rendered(trayIcon(true, true, 1, 3));
    const QImage absent = rendered(trayIcon(false, false, -1, 0));

    QVERIFY(running != guard);
    QVERIFY(running != absent);
    QVERIFY(guard != absent);

    // Nothing is answering, so there is no mode to show: the picture is the grey
    // one, and the sun in it is not warm.
    QCOMPARE(sunlight(absent), 0);
    QVERIFY(cloudPixels(absent) > 0);

    // The channels are independent: the guard changes the plate and leaves the
    // cloud where it was, so a clamped machine still says which envelope it is in.
    // Only the count of rim pixels moves, because the rim is blended with a plate
    // that is now a different colour.
    QCOMPARE(cloudBounds(guard), cloudBounds(running));
    QVERIFY(qAbs(cloudPixels(guard) - cloudPixels(running)) <= 4);
    QVERIFY(rendered(trayIcon(true, true, 0, 3)) != guard);
    QVERIFY(rendered(trayIcon(true, true, 2, 3)) != guard);
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

void TrayIconTest::nothingKnownAboutTheModeIsOnePicture()
{
    // A daemon that is not answering, a reply with no mode in it, and an empty
    // table are one picture. The icon speaks about the mode, and none of the
    // three knows what the mode is; which of them it is belongs to the tooltip.
    const QImage absent = rendered(trayIcon(false, false, -1, 0));
    const QImage noMode = rendered(trayIcon(true, false, -1, 3));
    const QImage noTable = rendered(trayIcon(true, false, 0, 0));

    QVERIFY(!absent.isNull());
    QCOMPARE(noMode, absent);
    QCOMPARE(noTable, absent);

    // And it is not any of the modes: the grey says so where a mode would be warm.
    QVERIFY(absent != rendered(trayIcon(true, false, 0, 3)));
    QVERIFY(absent != rendered(trayIcon(true, false, 1, 3)));
    QVERIFY(absent != rendered(trayIcon(true, false, 2, 3)));
    QVERIFY(sunlight(rendered(trayIcon(true, false, 1, 3))) > sunlight(absent));

    // The guard is still the plate, even with nothing known about the mode.
    QVERIFY(rendered(trayIcon(false, true, -1, 0)) != absent);
}

void TrayIconTest::aTableOfAnySizeReadsAsAPositionInIt()
{
    // The ends of the table are the ends of the scale whatever its size, and
    // everything between them is the one seat that says "some way in". There are
    // three modes, so each of them has a seat of its own.
    QCOMPARE(rendered(trayIcon(true, false, 0, 5)), rendered(trayIcon(true, false, 0, 3)));
    QCOMPARE(rendered(trayIcon(true, false, 4, 5)), rendered(trayIcon(true, false, 2, 3)));
    QCOMPARE(rendered(trayIcon(true, false, 2, 5)), rendered(trayIcon(true, false, 1, 3)));
    // An index past the end is the last mode rather than a seat off the plate.
    QCOMPARE(rendered(trayIcon(true, false, 9, 3)), rendered(trayIcon(true, false, 2, 3)));
    // A table of one is the whole of the scale, so its mode is the widest.
    QCOMPARE(rendered(trayIcon(true, false, 0, 1)), rendered(trayIcon(true, false, 2, 3)));
}

QTEST_MAIN(TrayIconTest)
#include "tray_icon_test.moc"
