// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "fake_sysfs.h"
#include "sensors.h"

#include <QTest>

class SensorsTest : public QObject {
    Q_OBJECT

private slots:
    void readsEveryChannel();
    void firstTickHasNoPowerAndNoThrottleRate();
    void derivesPowerFromEnergyDeltas();
    void energyCounterWraparound();
    void aCounterRestartIsRecordedAsNoReading();
    void countsConnectedDisplays();
    void separatesCoreFromUncore();
    void missingSensorDegradesInsteadOfFailing();
    void malformedValueIsTreatedAsAbsent();
    void fallsBackToTheSecondFanSource();
    void throttleCountersAreReportedAsRates();
};

void SensorsTest::readsEveryChannel()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    SensorReader reader(sysfs);
    reader.resolve();

    QCOMPARE(reader.packageTempPath(),
             FakeSysfs::coretemp() + QStringLiteral("/temp1_input"));
    QCOMPARE(reader.fanPath(), FakeSysfs::asus() + QStringLiteral("/fan1_input"));

    const Sample s = reader.read(1000, 0);
    QCOMPARE(s.ts, uint32_t(1000));
    QCOMPARE(s.pkgTemp, int16_t(610));   // 61000 millidegrees
    QCOMPARE(s.boardTemp, int16_t(450));
    QCOMPARE(s.nvmeTemp, int16_t(388));  // 38850 millidegrees truncates to 38.8
    QCOMPARE(s.fanRpm, uint16_t(1900));
    QCOMPARE(s.cpuMhz, uint16_t(2400));
    QCOMPARE(s.gpuMhz, uint16_t(300));
    QCOMPARE(s.load1, uint16_t(61));
    QVERIFY(!reader.degraded());
}

void SensorsTest::firstTickHasNoPowerAndNoThrottleRate()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    SensorReader reader(sysfs);
    reader.resolve();

    const Sample s = reader.read(1000, 0);
    // A cumulative counter says nothing until it has been read twice.
    QCOMPARE(s.pkgPower, kAbsentValue);
    QCOMPARE(s.pkgThrottle, uint16_t(0));
    QVERIFY(!reader.degraded());
}

void SensorsTest::derivesPowerFromEnergyDeltas()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    SensorReader reader(sysfs);
    reader.resolve();
    reader.read(1000, 0);

    // 23 joules over one second is 23 W, recorded as 2300 centiwatts.
    tree.write(FakeSysfs::raplPackage() + QStringLiteral("/energy_uj"),
               QStringLiteral("%1").arg(1000000 + 23000000));
    const Sample s = reader.read(1001, 1000);
    QCOMPARE(s.pkgPower, uint16_t(2300));
}

void SensorsTest::energyCounterWraparound()
{
    // The pure helper first: this is the case that never shows up in a log
    // because it happens once every few hours and looks like a glitch.
    QCOMPARE(energyDelta(500, 100, 1000), uint64_t(400));
    QCOMPARE(energyDelta(50, 900, 1000), uint64_t(150));
    QCOMPARE(energyDelta(50, 900, 0), uint64_t(0));
    QCOMPARE(energyDelta(50, 900, 800), uint64_t(0));

    FakeSysfs tree;
    tree.write(FakeSysfs::raplPackage() + QStringLiteral("/max_energy_range_uj"),
               QStringLiteral("100000000"));
    tree.write(FakeSysfs::raplPackage() + QStringLiteral("/energy_uj"),
               QStringLiteral("99000000"));
    Sysfs sysfs(tree.root());
    SensorReader reader(sysfs);
    reader.resolve();
    reader.read(1000, 0);

    // Wraps past 100 J and lands at 2 J: 3 J were drawn in that second.
    tree.write(FakeSysfs::raplPackage() + QStringLiteral("/energy_uj"), QStringLiteral("2000000"));
    const Sample s = reader.read(1001, 1000);
    QCOMPARE(s.pkgPower, uint16_t(300));
}

void SensorsTest::aCounterRestartIsRecordedAsNoReading()
{
    // What a resume from suspend does: the counter starts again from near zero,
    // the backwards step is read as a wrap, and one tick's worth of energy
    // becomes almost the whole range. Four samples of a recorded session
    // claimed 655.34 W — the top of the field — while the cores drew 0.7 W.
    // Absent is the honest answer; a saturated peak and a tidy zero are both
    // inventions.
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    SensorReader reader(sysfs);
    reader.resolve();
    reader.read(1000, 0);

    tree.write(FakeSysfs::raplPackage() + QStringLiteral("/energy_uj"), QStringLiteral("40000"));
    const Sample s = reader.read(1001, 1000);
    QCOMPARE(s.pkgPower, kAbsentValue);
    // The same tick's other readings are untouched: one artefact does not make
    // the whole sample unusable.
    QVERIFY(s.pkgTemp != kAbsentTemp);
}

void SensorsTest::countsConnectedDisplays()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    SensorReader reader(sysfs);
    reader.resolve();
    QCOMPARE(reader.read(1000, 0).displays, 1);

    tree.write(FakeSysfs::connector(QStringLiteral("DP-1")), QStringLiteral("connected"));
    tree.write(FakeSysfs::connector(QStringLiteral("HDMI-A-1")), QStringLiteral("connected"));
    QCOMPARE(reader.read(1001, 1000).displays, 3);

    // Everything unplugged, lid closed: zero is a legal answer and differs from
    // having nothing to ask.
    tree.write(FakeSysfs::connector(QStringLiteral("eDP-1")), QStringLiteral("disconnected"));
    tree.write(FakeSysfs::connector(QStringLiteral("DP-1")), QStringLiteral("disconnected"));
    tree.write(FakeSysfs::connector(QStringLiteral("HDMI-A-1")), QStringLiteral("disconnected"));
    QCOMPARE(reader.read(1002, 2000).displays, 0);

    // A connector whose status cannot be read is skipped rather than counted.
    tree.write(FakeSysfs::connector(QStringLiteral("eDP-1")), QStringLiteral("connected"));
    tree.remove(FakeSysfs::connector(QStringLiteral("DP-1")));
    SensorReader again(sysfs);
    again.resolve();
    QCOMPARE(again.read(1003, 3000).displays, 1);

    FakeSysfs bare;
    bare.remove(FakeSysfs::connector(QStringLiteral("eDP-1")));
    bare.remove(FakeSysfs::connector(QStringLiteral("DP-1")));
    bare.remove(FakeSysfs::connector(QStringLiteral("HDMI-A-1")));
    Sysfs bareSysfs(bare.root());
    SensorReader without(bareSysfs);
    without.resolve();
    QCOMPARE(without.read(1000, 0).displays, kAbsentPercent);
}

void SensorsTest::separatesCoreFromUncore()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    SensorReader reader(sysfs);
    reader.resolve();
    reader.read(1000, 0);

    tree.write(FakeSysfs::raplCore() + QStringLiteral("/energy_uj"),
               QStringLiteral("%1").arg(400000 + 8000000));
    tree.write(FakeSysfs::raplUncore() + QStringLiteral("/energy_uj"),
               QStringLiteral("%1").arg(100000 + 5000000));
    const Sample s = reader.read(1001, 1000);
    // The split is what tells iGPU heat from core heat.
    QCOMPARE(s.corePower, uint16_t(800));
    QCOMPARE(s.uncorePower, uint16_t(500));
}

void SensorsTest::missingSensorDegradesInsteadOfFailing()
{
    FakeSysfs tree;
    tree.remove(FakeSysfs::coretemp() + QStringLiteral("/temp1_input"));
    tree.remove(FakeSysfs::asus() + QStringLiteral("/fan1_input"));
    tree.remove(FakeSysfs::acpiFan() + QStringLiteral("/fan1_input"));
    Sysfs sysfs(tree.root());
    SensorReader reader(sysfs);
    reader.resolve();

    const Sample s = reader.read(1000, 0);
    QCOMPARE(s.pkgTemp, kAbsentTemp);
    QCOMPARE(s.fanRpm, kAbsentValue);
    QVERIFY(reader.degraded());
    QVERIFY(s.flags & FlagSensorsDegraded);
    // The rest of the record is still usable.
    QCOMPARE(s.boardTemp, int16_t(450));
}

void SensorsTest::malformedValueIsTreatedAsAbsent()
{
    FakeSysfs tree;
    tree.write(FakeSysfs::coretemp() + QStringLiteral("/temp1_input"), QStringLiteral("garbage"));
    tree.write(QStringLiteral("proc/loadavg"), QStringLiteral("not-a-number 0.1 0.2"));
    Sysfs sysfs(tree.root());
    SensorReader reader(sysfs);
    reader.resolve();

    const Sample s = reader.read(1000, 0);
    QCOMPARE(s.pkgTemp, kAbsentTemp);
    QCOMPARE(s.load1, kAbsentValue);
    QVERIFY(reader.degraded());
}

void SensorsTest::fallsBackToTheSecondFanSource()
{
    FakeSysfs tree;
    tree.remove(FakeSysfs::asus() + QStringLiteral("/fan1_input"));
    tree.write(FakeSysfs::acpiFan() + QStringLiteral("/fan1_input"), QStringLiteral("3700"));
    Sysfs sysfs(tree.root());
    SensorReader reader(sysfs);
    reader.resolve();

    QCOMPARE(reader.fanPath(), FakeSysfs::acpiFan() + QStringLiteral("/fan1_input"));
    QCOMPARE(reader.read(1000, 0).fanRpm, uint16_t(3700));
}

void SensorsTest::throttleCountersAreReportedAsRates()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    SensorReader reader(sysfs);
    reader.resolve();
    tree.write(FakeSysfs::cpu(0) + QStringLiteral("/thermal_throttle/package_throttle_count"),
               QStringLiteral("2178"));
    for (int i = 0; i < FakeSysfs::kCpuCount; ++i) {
        tree.write(FakeSysfs::cpu(i) + QStringLiteral("/thermal_throttle/core_throttle_count"),
                   QStringLiteral("100"));
    }
    // The counters are cumulative for the boot, so a large first reading means
    // nothing on its own.
    QCOMPARE(reader.read(1000, 0).pkgThrottle, uint16_t(0));

    tree.write(FakeSysfs::cpu(0) + QStringLiteral("/thermal_throttle/package_throttle_count"),
               QStringLiteral("2181"));
    const Sample s = reader.read(1001, 1000);
    QCOMPARE(s.pkgThrottle, uint16_t(3));
    QCOMPARE(s.coreThrottle, uint16_t(0));
}

QTEST_MAIN(SensorsTest)
#include "sensors_test.moc"
