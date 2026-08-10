// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "theme.h"

#include <QGuiApplication>
#include <QStandardPaths>
#include <QTest>

// The portal half of this module needs a session bus and a desktop portal, so
// what is tested here is the half that decides: which scheme a palette is, and
// what a palette for a scheme looks like. Those are the two places a mistake
// would show as a window nobody can read.
class ThemeTest : public QObject {
    Q_OBJECT

private slots:
    void aPaletteIsClassifiedByItsWindowColour();
    void luminanceIsWeightedRatherThanAveraged();
    void eachPaletteReadsAsItsOwnScheme();
    void textAndBackgroundAlwaysContrast();
    void anUnknownSchemeIsNotAPalette();
    void theStoredChoiceSurvivesAndAnythingElseIsTheDefault();
    void aNamedChoiceIsAppliedAndTheSameChoiceTwiceIsNoWork();
};

void ThemeTest::aPaletteIsClassifiedByItsWindowColour()
{
    QPalette dark;
    dark.setColor(QPalette::Window, QColor(0x2b, 0x2b, 0x2b));
    QCOMPARE(theme::schemeOf(dark), theme::Scheme::Dark);

    QPalette light;
    light.setColor(QPalette::Window, QColor(0xf2, 0xf1, 0xf0));
    QCOMPARE(theme::schemeOf(light), theme::Scheme::Light);
}

void ThemeTest::luminanceIsWeightedRatherThanAveraged()
{
    // Both have the same channel average. Green is the channel the eye is most
    // sensitive to, so a saturated green window is a light one and a saturated
    // blue window is a dark one — a plain average would call them the same.
    QPalette green;
    green.setColor(QPalette::Window, QColor(0, 200, 0));
    QCOMPARE(theme::schemeOf(green), theme::Scheme::Light);

    QPalette blue;
    blue.setColor(QPalette::Window, QColor(0, 0, 200));
    QCOMPARE(theme::schemeOf(blue), theme::Scheme::Dark);
}

void ThemeTest::eachPaletteReadsAsItsOwnScheme()
{
    // This is what keeps applyDesktopScheme() from swapping the palette on
    // every signal: it compares what it built against what is in force.
    QCOMPARE(theme::schemeOf(theme::paletteFor(theme::Scheme::Dark)), theme::Scheme::Dark);
    QCOMPARE(theme::schemeOf(theme::paletteFor(theme::Scheme::Light)), theme::Scheme::Light);
}

void ThemeTest::textAndBackgroundAlwaysContrast()
{
    for (const theme::Scheme scheme : { theme::Scheme::Dark, theme::Scheme::Light }) {
        const QPalette palette = theme::paletteFor(scheme);
        for (const QPalette::ColorRole role : { QPalette::WindowText, QPalette::Text,
                                                QPalette::ButtonText }) {
            const QColor background = palette.color(role == QPalette::Text ? QPalette::Base
                                                                          : QPalette::Window);
            const QColor foreground = palette.color(role);
            // A console whose text matches its background is worse than one
            // that ignored the desktop entirely.
            QVERIFY2(std::abs(background.lightness() - foreground.lightness()) > 100,
                     qPrintable(QStringLiteral("role %1 in scheme %2")
                                        .arg(int(role))
                                        .arg(int(scheme))));
        }
        // Disabled text has to be dimmer than active text and still legible.
        const QColor active = palette.color(QPalette::Active, QPalette::WindowText);
        const QColor disabled = palette.color(QPalette::Disabled, QPalette::WindowText);
        QVERIFY(active != disabled);
    }
}

void ThemeTest::anUnknownSchemeIsNotAPalette()
{
    // A desktop that will not say what it wants gets nothing applied, so the
    // platform theme's own palette stands. paletteFor() is never called with
    // Unknown; if it ever is, it must not produce something unreadable.
    const QPalette fallback = theme::paletteFor(theme::Scheme::Unknown);
    QVERIFY(std::abs(fallback.color(QPalette::Window).lightness()
                     - fallback.color(QPalette::WindowText).lightness())
            > 100);
}

QTEST_MAIN(ThemeTest)
void ThemeTest::theStoredChoiceSurvivesAndAnythingElseIsTheDefault()
{
    // Written under the test prefix rather than into the person's own
    // configuration: this writes a file, and it is not the test's file to own.
    QStandardPaths::setTestModeEnabled(true);

    theme::setPreference(theme::Preference::Dark);
    QCOMPARE(theme::preference(), theme::Preference::Dark);
    theme::setPreference(theme::Preference::Light);
    QCOMPARE(theme::preference(), theme::Preference::Light);
    theme::setPreference(theme::Preference::System);
    QCOMPARE(theme::preference(), theme::Preference::System);

    // Following the desktop is what an absent, empty or mistyped value means:
    // that is the answer that surprises nobody.
    QCOMPARE(theme::preferenceFrom(QString()), theme::Preference::System);
    QCOMPARE(theme::preferenceFrom(QStringLiteral("Dusk")), theme::Preference::System);
    QCOMPARE(theme::preferenceFrom(QStringLiteral("  DARK  ")), theme::Preference::Dark);
    QCOMPARE(theme::nameOf(theme::Preference::Light), QStringLiteral("light"));
}

void ThemeTest::aNamedChoiceIsAppliedAndTheSameChoiceTwiceIsNoWork()
{
    QStandardPaths::setTestModeEnabled(true);
    const QPalette original = QGuiApplication::palette();

    theme::setPreference(theme::Preference::Dark);
    QVERIFY(theme::applyPreference() || theme::schemeOf(QGuiApplication::palette())
                    == theme::Scheme::Dark);
    QCOMPARE(theme::schemeOf(QGuiApplication::palette()), theme::Scheme::Dark);
    // Nothing to do the second time: a palette that already agrees is left
    // alone, which is what keeps the window looking like the rest of the machine
    // on a correctly configured desktop.
    QVERIFY(!theme::applyPreference());

    theme::setPreference(theme::Preference::Light);
    QVERIFY(theme::applyPreference());
    QCOMPARE(theme::schemeOf(QGuiApplication::palette()), theme::Scheme::Light);

    theme::setPreference(theme::Preference::System);
    QGuiApplication::setPalette(original);
}

#include "theme_test.moc"
