// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "setpoints.h"

#include <QTest>

namespace {

Sample at(int16_t deciCelsius)
{
    Sample s {};
    s.pkgTemp = deciCelsius;
    return s;
}

Setpoints working()
{
    Setpoints points;
    points.profile = Profile::Balanced;
    points.pl1W = 20;
    points.pl2W = 30;
    points.maxPerfPct = 50;
    points.noTurbo = true;
    points.epp = Epp::Power;
    points.hwpBoost = false;
    points.gtMaxMhz = 2350;
    points.fanMode = 2;
    return points;
}

} // namespace

class SetpointsTest : public QObject {
    Q_OBJECT

private slots:
    void guardIsInertBelowItsThreshold();
    void guardClampsAtTheClampThreshold();
    void guardForcesTheFanOnlyAtItsOwnThreshold();
    void guardReleasesOnlyAfterTheDelay();
    void guardResetsTheDelayOnAReheat();
    void guardNeitherEscalatesNorReleasesWithoutATemperature();
    void guardOnlyLowers();
    void safeExitStaysWithinConfirmedLimits();
    void enumRoundTrips();
};

void SetpointsTest::guardIsInertBelowItsThreshold()
{
    GuardState state;
    const Setpoints wanted = working();
    const Setpoints effective = applyGuard(at(kGuardClampTempDeci - 1), wanted, state);
    QVERIFY(!state.engaged);
    QVERIFY(!state.fanForced);
    QCOMPARE(effective, wanted);
}

void SetpointsTest::guardClampsAtTheClampThreshold()
{
    GuardState state;
    const Setpoints effective = applyGuard(at(kGuardClampTempDeci), working(), state);
    QVERIFY(state.engaged);
    QVERIFY(!state.fanForced);
    QCOMPARE(effective.pl1W, kGuardPl1W);
    QCOMPARE(effective.pl2W, kGuardPl2W);
    QCOMPARE(effective.maxPerfPct, kMinPerfPct);
    QVERIFY(effective.noTurbo);
    QCOMPARE(effective.epp, Epp::Power);
    QVERIFY(!effective.hwpBoost);
    QCOMPARE(effective.gtMaxMhz, kMinGtMhz);
    // The fan is untouched until its own, higher threshold.
    QCOMPARE(effective.fanMode, 2);
}

void SetpointsTest::guardForcesTheFanOnlyAtItsOwnThreshold()
{
    GuardState state;
    Setpoints effective = applyGuard(at(kGuardFanMaxTempDeci - 1), working(), state);
    QCOMPARE(effective.fanMode, 2);

    effective = applyGuard(at(kGuardFanMaxTempDeci), working(), state);
    QVERIFY(state.fanForced);
    QCOMPARE(effective.fanMode, 0);
}

void SetpointsTest::guardReleasesOnlyAfterTheDelay()
{
    GuardState state;
    applyGuard(at(kGuardFanMaxTempDeci), working(), state);
    QVERIFY(state.engaged);
    QVERIFY(state.fanForced);

    for (int i = 0; i < kGuardReleaseTicks - 1; ++i) {
        const Setpoints effective = applyGuard(at(kGuardReleaseTempDeci), working(), state);
        QVERIFY2(state.engaged, qPrintable(QStringLiteral("released after %1 ticks").arg(i + 1)));
        QCOMPARE(effective.pl1W, kGuardPl1W);
        QCOMPARE(effective.fanMode, 0);
    }

    const Setpoints released = applyGuard(at(kGuardReleaseTempDeci), working(), state);
    QVERIFY(!state.engaged);
    QVERIFY(!state.fanForced);
    QCOMPARE(released, working());
}

void SetpointsTest::guardResetsTheDelayOnAReheat()
{
    GuardState state;
    applyGuard(at(kGuardClampTempDeci), working(), state);
    for (int i = 0; i < kGuardReleaseTicks - 1; ++i)
        applyGuard(at(kGuardReleaseTempDeci), working(), state);
    QCOMPARE(state.coolTicks, kGuardReleaseTicks - 1);

    // One tick above the release threshold and the count starts over, so the
    // clamp cannot chatter around its own boundary.
    applyGuard(at(kGuardReleaseTempDeci + 1), working(), state);
    QCOMPARE(state.coolTicks, 0);
    QVERIFY(state.engaged);

    for (int i = 0; i < kGuardReleaseTicks - 1; ++i)
        applyGuard(at(kGuardReleaseTempDeci), working(), state);
    QVERIFY(state.engaged);
    applyGuard(at(kGuardReleaseTempDeci), working(), state);
    QVERIFY(!state.engaged);
}

void SetpointsTest::guardNeitherEscalatesNorReleasesWithoutATemperature()
{
    GuardState state;
    Sample blind {};
    blind.pkgTemp = kAbsentTemp;

    applyGuard(blind, working(), state);
    QVERIFY(!state.engaged);

    applyGuard(at(kGuardFanMaxTempDeci), working(), state);
    QVERIFY(state.engaged);

    for (int i = 0; i < kGuardReleaseTicks * 3; ++i) {
        const Setpoints effective = applyGuard(blind, working(), state);
        QVERIFY(state.engaged);
        QCOMPARE(effective.fanMode, 0);
    }
}

void SetpointsTest::guardOnlyLowers()
{
    GuardState state;
    Setpoints already = working();
    already.pl1W = kGuardPl1W - 4;
    already.pl2W = kGuardPl2W - 4;
    already.maxPerfPct = kMinPerfPct;
    already.gtMaxMhz = kMinGtMhz;

    const Setpoints effective = applyGuard(at(kGuardClampTempDeci), already, state);
    QCOMPARE(effective.pl1W, already.pl1W);
    QCOMPARE(effective.pl2W, already.pl2W);
    QCOMPARE(effective.gtMaxMhz, already.gtMaxMhz);
}

void SetpointsTest::safeExitStaysWithinConfirmedLimits()
{
    const Setpoints exit = safeExitSetpoints();
    QCOMPARE(exit.pl1W, kMaxConfirmedPl1W);
    QCOMPARE(exit.pl2W, kMaxConfirmedPl2W);
    // The fan goes back to EC control; leaving it forced is the defect this
    // whole path exists to prevent.
    QCOMPARE(exit.fanMode, 2);
    // Never the firmware's own 200 W.
    QVERIFY(exit.pl1W < 200);
}

void SetpointsTest::enumRoundTrips()
{
    for (const Epp epp : { Epp::Performance, Epp::BalancePerformance,
                           Epp::BalancePower, Epp::Power })
        QCOMPARE(eppFromString(eppToString(epp)), epp);
    QCOMPARE(eppFromString(QStringLiteral("nonsense")), Epp::Unknown);
    QCOMPARE(eppToString(Epp::Unknown), QStringLiteral("unknown"));
    // "default" is refused rather than accepted: the node takes it and then
    // reports the preference it actually chose, which would look like another
    // process writing the control on every single tick.
    QCOMPARE(eppFromString(QStringLiteral("default")), Epp::Unknown);
    // The name still renders, because a stored sample carries the number.
    QCOMPARE(eppToString(Epp::Default), QStringLiteral("default"));

    for (const Profile profile : { Profile::Quiet, Profile::Balanced, Profile::Performance })
        QCOMPARE(profileFromString(profileToString(profile)), profile);
    QCOMPARE(profileFromString(QStringLiteral("nonsense")), Profile::Unknown);
    QCOMPARE(profileToString(Profile::Unknown), QStringLiteral("unknown"));

    for (const Mark mark : { Mark::Good, Mark::TooLoud, Mark::TooSlow })
        QCOMPARE(markFromString(markToString(mark)), mark);
    QCOMPARE(markFromString(QStringLiteral("nonsense")), Mark::None);
    QCOMPARE(markToString(Mark::None), QStringLiteral("none"));

    QVERIFY(working() != safeExitSetpoints());
}

QTEST_MAIN(SetpointsTest)
#include "setpoints_test.moc"
