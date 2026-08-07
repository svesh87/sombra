// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "brightness_map.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

namespace {

// A profile whose arithmetic is easy to check by hand: the curve maps the
// ambient reading straight onto a position, so with a base of 80 and a span of
// 10 the window is 70..90 and a reading of L asks for 70 + L/5.
BrightnessProfile testProfile()
{
    BrightnessProfile profile;
    profile.id = QStringLiteral("test");
    profile.spanPct = 10;
    profile.floorPct = 5;
    profile.ceilingPct = 100;
    profile.curve = { { 0, 0 }, { 100, 100 } };
    return profile;
}

// Ticks the controller and counts how many times it asked to move, which is
// how many writes the backlight would have seen.
int moves(const BrightnessProfile &profile, BrightnessState &state, int light, int ticks)
{
    int count = 0;
    for (int i = 0; i < ticks; ++i) {
        if (decideBrightness(profile, light, state).move)
            ++count;
    }
    return count;
}

} // namespace

class BrightnessMapTest : public QObject {
    Q_OBJECT

private slots:
    void theSpanIsMeasuredFromTheBaseSymmetrically();
    void theBoundsCutTheWindowRatherThanMovingIt();
    void aBaseOutsideTheBoundsIsStillAccepted();
    void theCurveInterpolatesAndIsFlatBeyondItsEnds();
    void theCurveChoosesAPointInsideTheWindow();
    void narrowingTheSpanReducesTheTravel();
    void theFirstDecisionAdoptsAPositionWithoutMoving();
    void steadyLightProducesNoWritesAtAll();
    void movementInsideOneStepProducesNoWrite();
    void noiseAtAStepBoundaryDoesNotSwitch();
    void aBurstShorterThanTheDwellIsIgnoredEntirely();
    void aSustainedChangeMovesOnceTheDwellHasPassed();
    void aMissingReadingHoldsTheLevel();
    void anUnknownBaseDecidesNothing();
    void profilesAreReadFromTheConfigurationFile();
    void abrokenProfileFallsBackRatherThanVanishing();
    void aFileWithoutProfilesLeavesTheTableAlone();

private:
    QString writeConfig(const QString &contents) const;

    QTemporaryDir m_work;
};

QString BrightnessMapTest::writeConfig(const QString &contents) const
{
    const QString path = m_work.path() + QStringLiteral("/modes.conf");
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(contents.toUtf8());
    file.close();
    return path;
}

void BrightnessMapTest::theSpanIsMeasuredFromTheBaseSymmetrically()
{
    const BrightnessProfile profile = testProfile();
    const BrightnessWindow window = brightnessWindow(profile, 80);
    QCOMPARE(window.low, 70);
    QCOMPARE(window.high, 90);
}

void BrightnessMapTest::theBoundsCutTheWindowRatherThanMovingIt()
{
    // The maintainer's own example: a base of 90 with a span of 10 and a
    // ceiling of 95 gives 80..95. The ceiling trims the top and leaves the
    // bottom where the span put it.
    BrightnessProfile profile = testProfile();
    profile.ceilingPct = 95;
    const BrightnessWindow window = brightnessWindow(profile, 90);
    QCOMPARE(window.low, 80);
    QCOMPARE(window.high, 95);

    profile.floorPct = 20;
    const BrightnessWindow low = brightnessWindow(profile, 25);
    QCOMPARE(low.low, 20);
    QCOMPARE(low.high, 35);
}

void BrightnessMapTest::aBaseOutsideTheBoundsIsStillAccepted()
{
    // The bounds constrain the automation, never the person. A level set by
    // hand above the ceiling is accepted as the base, and the automation still
    // works inside the permitted range.
    BrightnessProfile profile = testProfile();
    profile.ceilingPct = 60;
    const BrightnessWindow window = brightnessWindow(profile, 100);
    QCOMPARE(window.high, 60);
    QVERIFY(window.low <= window.high);

    BrightnessState state;
    state.base = 100;
    decideBrightness(profile, 100, state);
    const BrightnessDecision decision = decideBrightness(profile, 0, state);
    QVERIFY(decision.window.high <= profile.ceilingPct);
    QVERIFY(decision.window.low >= profile.floorPct);
}

void BrightnessMapTest::theCurveInterpolatesAndIsFlatBeyondItsEnds()
{
    const QList<BrightnessPoint> curve = { { 100, 10 }, { 200, 50 }, { 400, 90 } };
    QCOMPARE(brightnessCurvePosition(curve, 50), 10);
    QCOMPARE(brightnessCurvePosition(curve, 100), 10);
    QCOMPARE(brightnessCurvePosition(curve, 150), 30);
    QCOMPARE(brightnessCurvePosition(curve, 300), 70);
    QCOMPARE(brightnessCurvePosition(curve, 400), 90);
    QCOMPARE(brightnessCurvePosition(curve, 9000), 90);
    QCOMPARE(brightnessCurvePosition({}, 500), 0);
}

void BrightnessMapTest::theCurveChoosesAPointInsideTheWindow()
{
    const BrightnessProfile profile = testProfile();
    BrightnessState state;
    state.base = 80;

    // Darkness lands on the bottom of the window, not on the bottom of the
    // scale: the curve chooses a position in the window.
    decideBrightness(profile, 0, state);
    QCOMPARE(state.step, 70);

    BrightnessState bright;
    bright.base = 80;
    decideBrightness(profile, 100, bright);
    QCOMPARE(bright.step, 90);
}

void BrightnessMapTest::narrowingTheSpanReducesTheTravel()
{
    // The alternative — an absolute curve that the window clamps — would pin a
    // narrow window to one edge and stop responding to light at all.
    BrightnessProfile narrow = testProfile();
    narrow.spanPct = 5;

    BrightnessState dark;
    dark.base = 80;
    decideBrightness(narrow, 0, dark);
    BrightnessState light;
    light.base = 80;
    decideBrightness(narrow, 100, light);

    QCOMPARE(dark.step, 75);
    QCOMPARE(light.step, 85);
    QVERIFY(light.step > dark.step);
}

void BrightnessMapTest::theFirstDecisionAdoptsAPositionWithoutMoving()
{
    const BrightnessProfile profile = testProfile();
    BrightnessState state;
    state.base = 80;

    const BrightnessDecision decision = decideBrightness(profile, 100, state);
    QVERIFY(!decision.move);
    // The backlight stays where the person left it; only the position is
    // adopted, so the next move waits for the light itself to change.
    QCOMPARE(decision.target, 80);
    QCOMPARE(state.step, 90);
}

void BrightnessMapTest::steadyLightProducesNoWritesAtAll()
{
    const BrightnessProfile profile = testProfile();
    BrightnessState state;
    state.base = 80;
    decideBrightness(profile, 50, state);

    // An hour of unchanging light. Anything above zero here is the backlight
    // being written for no reason.
    QCOMPARE(moves(profile, state, 50, 3600), 0);
}

void BrightnessMapTest::movementInsideOneStepProducesNoWrite()
{
    const BrightnessProfile profile = testProfile();
    BrightnessState state;
    state.base = 80;
    decideBrightness(profile, 50, state);
    QCOMPARE(state.step, 80);

    // 50 asks for 80 and 60 asks for 82, both inside the same coarse step.
    for (const int light : { 48, 52, 55, 58, 60 })
        QCOMPARE(moves(profile, state, light, 20), 0);
}

void BrightnessMapTest::noiseAtAStepBoundaryDoesNotSwitch()
{
    const BrightnessProfile profile = testProfile();
    BrightnessState state;
    state.base = 80;
    decideBrightness(profile, 50, state);

    // 65 asks for 83, which rounds to the next step but has not cleared the
    // boundary by the hysteresis margin. Without that margin the coarse grid
    // would only look like it was working.
    int count = 0;
    for (int i = 0; i < 100; ++i)
        count += moves(profile, state, i % 2 == 0 ? 65 : 60, 1);
    QCOMPARE(count, 0);
    QCOMPARE(state.step, 80);
}

void BrightnessMapTest::aBurstShorterThanTheDwellIsIgnoredEntirely()
{
    const BrightnessProfile profile = testProfile();
    BrightnessState state;
    state.base = 80;
    decideBrightness(profile, 0, state);
    QCOMPARE(state.step, 70);

    // A hand passing over the sensor, or headlights.
    QCOMPARE(moves(profile, state, 100, kBrightnessDwellTicks - 1), 0);
    QCOMPARE(moves(profile, state, 0, 5), 0);
    QCOMPARE(state.step, 70);
}

void BrightnessMapTest::aSustainedChangeMovesOnceTheDwellHasPassed()
{
    const BrightnessProfile profile = testProfile();
    BrightnessState state;
    state.base = 80;
    decideBrightness(profile, 0, state);

    QCOMPARE(moves(profile, state, 100, kBrightnessDwellTicks - 1), 0);
    const BrightnessDecision decision = decideBrightness(profile, 100, state);
    QVERIFY(decision.move);
    QCOMPARE(decision.target, 90);
    QCOMPARE(state.step, 90);

    // And exactly once: the new step is where we now are.
    QCOMPARE(moves(profile, state, 100, 100), 0);
}

void BrightnessMapTest::aMissingReadingHoldsTheLevel()
{
    const BrightnessProfile profile = testProfile();
    BrightnessState state;
    state.base = 80;
    decideBrightness(profile, 0, state);

    // No reading is not darkness.
    for (int i = 0; i < 50; ++i)
        QVERIFY(!decideBrightness(profile, std::nullopt, state).move);
    QCOMPARE(state.step, 70);

    // Nor is a profile without a usable curve.
    BrightnessProfile empty = testProfile();
    empty.curve.clear();
    QVERIFY(!decideBrightness(empty, 100, state).move);
}

void BrightnessMapTest::anUnknownBaseDecidesNothing()
{
    const BrightnessProfile profile = testProfile();
    BrightnessState state;
    const BrightnessDecision decision = decideBrightness(profile, 100, state);
    QVERIFY(!decision.move);
    QCOMPARE(state.step, BrightnessState::kUnset);
}

void BrightnessMapTest::profilesAreReadFromTheConfigurationFile()
{
    const QString path = writeConfig(QStringLiteral(
            "[general]\n"
            "default_brightness_profile=evening\n"
            "[brightness evening]\n"
            "label=Evening\n"
            "span_pct=15\n"
            "floor_pct=10\n"
            "ceiling_pct=80\n"
            "curve=0:0, 250:40, 1000:100\n"
            "[brightness daylight]\n"
            "span_pct=5\n"
            "curve=0:0, 5000:100\n"));

    BrightnessProfileTable table;
    QStringList problems;
    QVERIFY(table.load(path, &problems));
    QCOMPARE(table.profiles().size(), 2);
    QCOMPARE(table.defaultProfileId(), QStringLiteral("evening"));
    QCOMPARE(table.indexOf(QStringLiteral("evening")), 1);
    QCOMPARE(table.indexOf(QStringLiteral("absent")), -1);

    const BrightnessProfile *evening = table.find(QStringLiteral("evening"));
    QVERIFY(evening);
    QCOMPARE(evening->label, QStringLiteral("Evening"));
    QCOMPARE(evening->spanPct, 15);
    QCOMPARE(evening->floorPct, 10);
    QCOMPARE(evening->ceilingPct, 80);
    QCOMPARE(evening->curve.size(), 3);
    QCOMPARE(brightnessCurvePosition(evening->curve, 250), 40);
}

void BrightnessMapTest::abrokenProfileFallsBackRatherThanVanishing()
{
    const QString path = writeConfig(QStringLiteral("[general]\n"
                                                    "default_brightness_profile=missing\n"
                                                    "[brightness ]\n"
                                                    "curve=0:0\n"
                                                    "[brightness broken]\n"
                                                    "span_pct=400\n"
                                                    "floor_pct=-10\n"
                                                    "ceiling_pct=999\n"
                                                    "curve=nonsense, 5:notanumber, 10:20\n"));

    BrightnessProfileTable table;
    QStringList problems;
    QVERIFY(table.load(path, &problems));
    QCOMPARE(table.profiles().size(), 1);

    const BrightnessProfile *broken = table.find(QStringLiteral("broken"));
    QVERIFY(broken);
    QCOMPARE(broken->spanPct, 100);
    QCOMPARE(broken->floorPct, 0);
    QCOMPARE(broken->ceilingPct, 100);
    // One usable point survived; the rest were reported rather than swallowed.
    QCOMPARE(broken->curve.size(), 1);
    QVERIFY(problems.size() >= 4);

    // A default naming a profile that does not exist falls back to a real one
    // instead of leaving the automation without a window.
    QCOMPARE(table.defaultProfileId(), QStringLiteral("broken"));
}

void BrightnessMapTest::aFileWithoutProfilesLeavesTheTableAlone()
{
    BrightnessProfileTable table = BrightnessProfileTable::builtinDefaults();
    QCOMPARE(table.profiles().size(), 1);

    QVERIFY(!table.load(m_work.path() + QStringLiteral("/absent.conf")));
    QVERIFY(!table.load(writeConfig(QStringLiteral("[mode work]\npl1_w=20\n"))));
    QCOMPARE(table.profiles().size(), 1);
    QCOMPARE(table.defaultProfileId(), QStringLiteral("indoors"));

    // The built-in curve rises with the light and never leaves 0..100.
    const BrightnessProfile &indoors = table.profiles().first();
    QCOMPARE(brightnessCurvePosition(indoors.curve, 0), 0);
    QCOMPARE(brightnessCurvePosition(indoors.curve, 100000), 100);
    QVERIFY(brightnessCurvePosition(indoors.curve, 1000)
            < brightnessCurvePosition(indoors.curve, 4000));
}

QTEST_MAIN(BrightnessMapTest)
#include "brightness_map_test.moc"
