// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "brightness_map.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

namespace {

// Millilux, since that is what the model works in. Written as lux times a
// thousand so the numbers in the cases below read as rooms.
constexpr quint32 kDark = 300;        // 0.3 lux, the darkest recorded night
constexpr quint32 kEvening = 25000;   // 25 lux, desk lamps and no ceiling light
constexpr quint32 kCeiling = 103000;  // 103 lux, the same room lit up

// Settings whose arithmetic is easy to follow: the curve is neutral at the
// evening level, so a level set then is the base unchanged.
BrightnessSettings testSettings()
{
    BrightnessSettings settings;
    settings.responsePct = 30;
    settings.floorPct = 5;
    // The first point sits at the darkest recorded night rather than at zero:
    // below it the curve is flat, and that is the honest shape — 0.3 lux and
    // 0.0 lux are the same room with the lights off.
    settings.curve = { { kDark, 0 }, { kEvening, 50 }, { kCeiling, 100 } };
    return settings;
}

// Ticks the model and counts how many times it asked to move, which is how many
// writes the backlight would have seen.
int moves(const BrightnessSettings &settings, BrightnessState &state, quint32 light, int ticks)
{
    int count = 0;
    for (int i = 0; i < ticks; ++i) {
        if (decideBrightness(settings, light, state).move)
            ++count;
    }
    return count;
}

} // namespace

class BrightnessMapTest : public QObject {
    Q_OBJECT

private slots:
    void aBaselineSetOutrightReproducesItsLevel();
    void aTargetJustOverAGridBoundaryDoesNotMoveTheStep();
    void theCurveIsASignedContributionAroundItsNeutralPoint();
    void theBaseIsTheLevelWithTheLightTakenOut();
    void theSameLightReproducesTheLevelThatWasSet();
    void aLevelBelowTheFloorNeverBecomesTheBase();
    void theFloorAndTheTopOfTheScaleBoundTheResult();
    void theCeilingBoundsEverythingTheFloorDoesFromTheOtherSide();
    void aCeilingUnderTheFloorGivesWayToIt();
    void aWiderResponseMovesFurtherForTheSameLight();
    void theFirstDecisionAdoptsAPositionWithoutMoving();
    void steadyLightProducesNoWritesAtAll();
    void aBurstShorterThanTheDwellIsIgnoredEntirely();
    void aSustainedChangeMovesOnceTheDwellHasPassed();
    void aMissingReadingHoldsTheLevel();
    void anUnknownBaseDecidesNothing();
    void settingsAreReadFromTheFile();
    void theSectionReadsBackAsTheSameSettings();
    void curvePointsAreWrittenInLuxAndReadBackExactly();
    void theBuiltInCurveCoversTheRoomThatWasMeasured();

private:
    QString writeConfig(const QString &contents) const;

    QTemporaryDir m_work;
};

QString BrightnessMapTest::writeConfig(const QString &contents) const
{
    const QString path = m_work.path() + QStringLiteral("/settings.conf");
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(contents.toUtf8());
    file.close();
    return path;
}

void BrightnessMapTest::aTargetJustOverAGridBoundaryDoesNotMoveTheStep()
{
    // The defect this is here for: the hysteresis used to be measured on the
    // target after it had been snapped to the grid, where two adjacent steps are
    // always a whole step apart and clear any margin smaller than one. So the
    // margin never applied, the grid's boundary was a single point, and a light
    // drifting across it moved the backlight every few seconds. A morning by a
    // window produced forty-three moves in twelve hours.
    BrightnessSettings settings = builtinBrightnessSettings();
    settings.responsePct = 40;
    settings.floorPct = 5;

    BrightnessState state;
    state.base = 72;

    // Adopted without moving: 85 lux puts the target at 95, which is a step.
    BrightnessDecision decision = decideBrightness(settings, 85100, state);
    QVERIFY(!decision.move);
    QCOMPARE(state.step, 95);

    // 96 lux puts the target at 98 — over the halfway point to 100, so snapping
    // alone would move it, and the margin says it has not gone far enough.
    for (int tick = 0; tick < 4 * kBrightnessDwellTicks; ++tick) {
        decision = decideBrightness(settings, 95800, state);
        QVERIFY2(!decision.move, "a target inside the margin is the step it is in");
    }
    QCOMPARE(state.step, 95);

    // 103 lux puts it at 100, which clears the margin. It still has to hold for
    // the dwell, and then it moves.
    for (int tick = 1; tick < kBrightnessDwellTicks; ++tick)
        QVERIFY(!decideBrightness(settings, 103000, state).move);
    decision = decideBrightness(settings, 103000, state);
    QVERIFY(decision.move);
    QCOMPARE(decision.target, 100);
    QCOMPARE(state.step, 100);
}

void BrightnessMapTest::aBaselineSetOutrightReproducesItsLevel()
{
    const BrightnessSettings settings = builtinBrightnessSettings();
    // Two rooms, one baseline. The point of the baseline is that it is the level
    // the light is measured against, so returning to a light returns to a level.
    const quint32 evening = 25000;   // millilux: the room the curve calls neutral
    const quint32 bright = 103000;

    const int base = 70;
    const int inEvening = std::clamp(base + brightnessContribution(settings, evening),
                                     settings.floorPct, 100);
    const int inBright = std::clamp(base + brightnessContribution(settings, bright),
                                    settings.floorPct, 100);
    QVERIFY(inBright > inEvening);

    // And the derivation is the inverse: the level reached in a light, taken back
    // through that light, is the baseline it came from.
    QCOMPARE(brightnessBaseFor(settings, inEvening, evening), base);
    QCOMPARE(brightnessBaseFor(settings, inBright, bright), base);
}

void BrightnessMapTest::theCurveIsASignedContributionAroundItsNeutralPoint()
{
    const BrightnessSettings settings = testSettings();
    // Position 50 is the light at which the screen sits exactly on the level
    // the person set. Below it the room subtracts, above it the room adds.
    QCOMPARE(brightnessContribution(settings, kEvening), 0);
    QCOMPARE(brightnessContribution(settings, kDark), -30);
    QCOMPARE(brightnessContribution(settings, kCeiling), 30);
    // No reading contributes nothing rather than darkness.
    QCOMPARE(brightnessContribution(settings, std::nullopt), 0);
}

void BrightnessMapTest::theBaseIsTheLevelWithTheLightTakenOut()
{
    const BrightnessSettings settings = testSettings();
    // Set at the neutral light, the base is what was set.
    QCOMPARE(brightnessBaseFor(settings, 80, kEvening), 80);
    // Set in the dark, where the room was subtracting thirty, the base is
    // thirty higher — which is what makes a bright room later come out right.
    QCOMPARE(brightnessBaseFor(settings, 80, kDark), 110);
    QCOMPARE(brightnessBaseFor(settings, 80, kCeiling), 50);
}

void BrightnessMapTest::theSameLightReproducesTheLevelThatWasSet()
{
    // The whole point of the model, and the thing the older one could not do:
    // leave the room, come back to the same light, get the same screen.
    const BrightnessSettings settings = testSettings();
    BrightnessState state;
    state.base = brightnessBaseFor(settings, 80, kEvening);

    decideBrightness(settings, kEvening, state);
    moves(settings, state, kCeiling, 20);
    QCOMPARE(state.step, 100);
    moves(settings, state, kEvening, 20);
    QCOMPARE(state.step, 80);
}

void BrightnessMapTest::aLevelBelowTheFloorNeverBecomesTheBase()
{
    BrightnessSettings settings = testSettings();
    settings.floorPct = 20;
    // What got applied is what the base is derived from. A base built from a
    // level nobody can see would leave the automation working towards a number
    // that never appears on screen.
    QCOMPARE(brightnessBaseFor(settings, 2, kEvening), 20);
    QCOMPARE(brightnessBaseFor(settings, 2, kDark), 50);
}

void BrightnessMapTest::theFloorAndTheTopOfTheScaleBoundTheResult()
{
    BrightnessSettings settings = testSettings();
    settings.floorPct = 20;
    BrightnessState state;
    state.base = 30;

    decideBrightness(settings, kEvening, state);
    moves(settings, state, kDark, 20);
    // Thirty minus thirty is zero, and the floor is what actually happens.
    QCOMPARE(state.step, 20);

    BrightnessState high;
    high.base = 90;
    decideBrightness(settings, kEvening, high);
    moves(settings, high, kCeiling, 20);
    QCOMPARE(high.step, 100);

    // The window shown to the person is what will happen, not what the
    // arithmetic wanted.
    const BrightnessWindow window = brightnessWindow(settings, 90);
    QCOMPARE(window.low, 60);
    QCOMPARE(window.high, 100);
}

void BrightnessMapTest::theCeilingBoundsEverythingTheFloorDoesFromTheOtherSide()
{
    BrightnessSettings settings = testSettings();
    settings.ceilingPct = 60;
    BrightnessState state;
    state.base = 50;

    // The brightest room the curve knows adds the whole response, and the
    // ceiling is what actually happens.
    decideBrightness(settings, kEvening, state);
    moves(settings, state, kCeiling, 20);
    QCOMPARE(state.step, 60);

    // The window shown to the person is cut by it too.
    const BrightnessWindow window = brightnessWindow(settings, 50);
    QCOMPARE(window.low, 20);
    QCOMPARE(window.high, 60);

    // A level above the ceiling never becomes a base: what got applied is what
    // the base is derived from, exactly as with the floor.
    QCOMPARE(brightnessBaseFor(settings, 90, kEvening), 60);
    QCOMPARE(brightnessClamp(settings, 90), 60);
    QCOMPARE(brightnessClamp(settings, 1), settings.floorPct);
}

void BrightnessMapTest::aCeilingUnderTheFloorGivesWayToIt()
{
    // Through the settings vocabulary: the bound just given wins and the other
    // one is moved out of its way, with a line saying so.
    BrightnessSettings settings = testSettings();
    QString error;
    QStringList clamped;
    QVERIFY(applyBrightnessSetting(settings, QStringLiteral("ceiling_pct"),
                                   QStringLiteral("40"), &error, &clamped));
    QVERIFY(applyBrightnessSetting(settings, QStringLiteral("floor_pct"),
                                   QStringLiteral("70"), &error, &clamped));
    QCOMPARE(settings.floorPct, 70);
    QCOMPARE(settings.ceilingPct, 70);
    QVERIFY2(clamped.join(QLatin1Char(';')).contains(QStringLiteral("ceiling_pct raised")),
             qPrintable(clamped.join(QLatin1Char(';'))));

    clamped.clear();
    QVERIFY(applyBrightnessSetting(settings, QStringLiteral("ceiling_pct"),
                                   QStringLiteral("30"), &error, &clamped));
    QCOMPARE(settings.ceilingPct, 30);
    QCOMPARE(settings.floorPct, 30);
    QVERIFY2(clamped.join(QLatin1Char(';')).contains(QStringLiteral("floor_pct lowered")),
             qPrintable(clamped.join(QLatin1Char(';'))));

    // And a struct assembled by hand with the bounds crossed still answers
    // rather than running off the end of std::clamp.
    BrightnessSettings crossed = testSettings();
    crossed.floorPct = 80;
    crossed.ceilingPct = 10;
    QCOMPARE(brightnessClamp(crossed, 50), 80);
}

void BrightnessMapTest::aWiderResponseMovesFurtherForTheSameLight()
{
    BrightnessSettings narrow = testSettings();
    narrow.responsePct = 10;
    BrightnessSettings wide = testSettings();
    wide.responsePct = 50;

    QCOMPARE(brightnessContribution(narrow, kCeiling), 10);
    QCOMPARE(brightnessContribution(wide, kCeiling), 50);
    QCOMPARE(brightnessContribution(narrow, kDark), -10);
    QCOMPARE(brightnessContribution(wide, kDark), -50);
}

void BrightnessMapTest::theFirstDecisionAdoptsAPositionWithoutMoving()
{
    const BrightnessSettings settings = testSettings();
    BrightnessState state;
    state.base = 80;

    const BrightnessDecision decision = decideBrightness(settings, kCeiling, state);
    QVERIFY(!decision.move);
    // The backlight stays where the person left it; only the position is
    // adopted, so the next move waits for the light itself to change.
    QCOMPARE(decision.target, 80);
    QCOMPARE(state.step, 100);
}

void BrightnessMapTest::steadyLightProducesNoWritesAtAll()
{
    const BrightnessSettings settings = testSettings();
    BrightnessState state;
    state.base = 80;
    decideBrightness(settings, kEvening, state);

    // An hour of unchanging light. Anything above zero here is the backlight
    // being written for no reason.
    QCOMPARE(moves(settings, state, kEvening, 3600), 0);
}

void BrightnessMapTest::aBurstShorterThanTheDwellIsIgnoredEntirely()
{
    const BrightnessSettings settings = testSettings();
    BrightnessState state;
    state.base = 80;
    decideBrightness(settings, kEvening, state);

    // A hand passing over the sensor, or headlights through a window.
    QCOMPARE(moves(settings, state, kCeiling, kBrightnessDwellTicks - 1), 0);
    QCOMPARE(moves(settings, state, kEvening, 5), 0);
    QCOMPARE(state.step, 80);
}

void BrightnessMapTest::aSustainedChangeMovesOnceTheDwellHasPassed()
{
    const BrightnessSettings settings = testSettings();
    BrightnessState state;
    state.base = 80;
    decideBrightness(settings, kEvening, state);

    int ticks = 0;
    for (; ticks < 20; ++ticks) {
        if (decideBrightness(settings, kCeiling, state).move)
            break;
    }
    QCOMPARE(ticks, kBrightnessDwellTicks - 1);
    QCOMPARE(state.step, 100);
    // And exactly once: the move is not repeated every tick afterwards.
    QCOMPARE(moves(settings, state, kCeiling, 60), 0);
}

void BrightnessMapTest::aMissingReadingHoldsTheLevel()
{
    const BrightnessSettings settings = testSettings();
    BrightnessState state;
    state.base = 80;
    decideBrightness(settings, kEvening, state);

    // Darkness and a missing sensor are not the same thing.
    for (int i = 0; i < 100; ++i)
        QVERIFY(!decideBrightness(settings, std::nullopt, state).move);
    QCOMPARE(state.step, 80);
}

void BrightnessMapTest::anUnknownBaseDecidesNothing()
{
    const BrightnessSettings settings = testSettings();
    BrightnessState state;
    QVERIFY(!decideBrightness(settings, kEvening, state).move);
}

void BrightnessMapTest::settingsAreReadFromTheFile()
{
    BrightnessSettings settings = builtinBrightnessSettings();
    QStringList problems;
    QVERIFY(loadBrightnessSettings(writeConfig(QStringLiteral("[brightness]\n"
                                                             "response_pct=40\n"
                                                             "floor_pct=15\n"
                                                             "curve=0:0, 25:50, 400:100\n")),
                                   &settings, &problems));
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
    QCOMPARE(settings.responsePct, 40);
    QCOMPARE(settings.floorPct, 15);
    QCOMPARE(settings.curve.size(), 3);
    // Lux in the file, millilux inside.
    QCOMPARE(settings.curve.at(1).light, 25000u);

    // A file without the section leaves the settings alone, which is how a
    // fresh install runs on the built-in values.
    BrightnessSettings untouched = builtinBrightnessSettings();
    QVERIFY(!loadBrightnessSettings(writeConfig(QStringLiteral("[general]\n")), &untouched));
    QCOMPARE(untouched.responsePct, kBrightnessDefaultResponsePct);

    // One bad point is dropped and reported; the rest of the curve survives.
    BrightnessSettings partial = builtinBrightnessSettings();
    problems.clear();
    QVERIFY(loadBrightnessSettings(writeConfig(QStringLiteral("[brightness]\n"
                                                             "curve=0:0, nonsense, 400:100\n")),
                                   &partial, &problems));
    QCOMPARE(partial.curve.size(), 2);
    QCOMPARE(problems.size(), 1);
}

void BrightnessMapTest::theSectionReadsBackAsTheSameSettings()
{
    BrightnessSettings settings = builtinBrightnessSettings();
    settings.responsePct = 45;
    settings.floorPct = 12;

    // The daemon writes this section and reads it back at the next boot, so a
    // section that does not survive the round trip loses settings silently.
    BrightnessSettings read = builtinBrightnessSettings();
    QVERIFY(loadBrightnessSettings(writeConfig(brightnessSettingsSection(settings)), &read));
    QCOMPARE(read.responsePct, 45);
    QCOMPARE(read.floorPct, 12);
    QCOMPARE(read.curve.size(), settings.curve.size());
    for (int i = 0; i < read.curve.size(); ++i) {
        QCOMPARE(read.curve.at(i).light, settings.curve.at(i).light);
        QCOMPARE(read.curve.at(i).position, settings.curve.at(i).position);
    }
}

void BrightnessMapTest::curvePointsAreWrittenInLuxAndReadBackExactly()
{
    BrightnessSettings settings = builtinBrightnessSettings();
    // A tenth of a lux is a real difference at the dark end, and it has to
    // survive being written down.
    settings.curve = { { 300, 0 }, { 1500, 40 }, { 20855000, 100 } };
    const QString section = brightnessSettingsSection(settings);
    QVERIFY2(section.contains(QStringLiteral("0.3:0")), qPrintable(section));
    QVERIFY2(section.contains(QStringLiteral("20855:100")), qPrintable(section));

    BrightnessSettings read = builtinBrightnessSettings();
    QVERIFY(loadBrightnessSettings(writeConfig(section), &read));
    QCOMPARE(read.curve.at(0).light, 300u);
    QCOMPARE(read.curve.at(2).light, 20855000u);
}

void BrightnessMapTest::theBuiltInCurveCoversTheRoomThatWasMeasured()
{
    const BrightnessSettings settings = builtinBrightnessSettings();
    // A curve whose last point sits below the room's ordinary light answers
    // the same thing for every lit room there is. The last point has to be
    // above what the room actually reaches.
    QVERIFY(settings.curve.last().light > kCeiling);
    // And it has to be neutral somewhere sensible: the evening light of that
    // room, which is where the level the person sets should be left alone.
    QCOMPARE(brightnessContribution(settings, kEvening), 0);
    QVERIFY(brightnessContribution(settings, kDark) < 0);
    QVERIFY(brightnessContribution(settings, kCeiling) > 0);
}

QTEST_MAIN(BrightnessMapTest)
#include "brightness_map_test.moc"
