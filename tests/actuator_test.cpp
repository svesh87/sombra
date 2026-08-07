// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "actuator.h"
#include "fake_sysfs.h"

#include <QDateTime>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

namespace {

Setpoints quietPoints()
{
    Setpoints points;
    points.profile = Profile::Quiet;
    points.pl1W = 12;
    points.pl2W = 20;
    points.maxPerfPct = 40;
    points.noTurbo = true;
    points.epp = Epp::Power;
    points.hwpBoost = false;
    points.gtMaxMhz = 1000;
    points.fanMode = 2;
    return points;
}

} // namespace

class ActuatorTest : public QObject {
    Q_OBJECT

private slots:
    void writesEveryControlInTheRequiredOrder();
    void appliesToEveryCpuForEpp();
    void isIdempotent();
    void detectsAValueItDidNotWrite();
    void detectsANodeThatDisappeared();
    void refusesTheLazyCurveWithoutAPowerLimit();
    void reportsATreeItCannotManage();
    void missingNodesAreReportedNotFatal();
    void restorePutsTheFanBackOnEveryPath();
    void restoreNeverRaisesPastTheConfirmedCeiling();
};

void ActuatorTest::writesEveryControlInTheRequiredOrder()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    Actuator actuator(sysfs);
    actuator.resolve();

    actuator.apply(quietPoints());

    QCOMPARE(tree.read(FakeSysfs::platformProfile()), QStringLiteral("quiet"));
    // Microwatts, and written after the profile: writing the profile makes the
    // firmware reassert its own limits.
    QCOMPARE(tree.read(FakeSysfs::raplPackage() + QStringLiteral("/constraint_0_power_limit_uw")),
             QStringLiteral("12000000"));
    QCOMPARE(tree.read(FakeSysfs::raplPackage() + QStringLiteral("/constraint_1_power_limit_uw")),
             QStringLiteral("20000000"));
    QCOMPARE(tree.read(FakeSysfs::pstate() + QStringLiteral("/max_perf_pct")),
             QStringLiteral("40"));
    QCOMPARE(tree.read(FakeSysfs::pstate() + QStringLiteral("/no_turbo")), QStringLiteral("1"));
    QCOMPARE(tree.read(FakeSysfs::pstate() + QStringLiteral("/hwp_dynamic_boost")),
             QStringLiteral("0"));
    QCOMPARE(tree.read(FakeSysfs::card() + QStringLiteral("/gt_max_freq_mhz")),
             QStringLiteral("1000"));
    QCOMPARE(tree.read(FakeSysfs::asus() + QStringLiteral("/pwm1_enable")), QStringLiteral("2"));
    QVERIFY(!actuator.lastApplyMismatched());
}

void ActuatorTest::appliesToEveryCpuForEpp()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    Actuator actuator(sysfs);
    actuator.resolve();
    actuator.apply(quietPoints());

    for (int i = 0; i < FakeSysfs::kCpuCount; ++i) {
        QCOMPARE(tree.read(FakeSysfs::cpu(i)
                           + QStringLiteral("/cpufreq/energy_performance_preference")),
                 QStringLiteral("power"));
    }
}

void ActuatorTest::isIdempotent()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    Actuator actuator(sysfs);
    actuator.resolve();
    actuator.apply(quietPoints());

    const QString node = FakeSysfs::pstate() + QStringLiteral("/max_perf_pct");
    const QDateTime before = QFileInfo(tree.root() + QLatin1Char('/') + node).lastModified();

    // A node that already holds the wanted value is not written again, so the
    // steady state costs reads and no writes.
    tree.makeUnwritable(node);
    actuator.apply(quietPoints());
    QVERIFY(!actuator.lastApplyMismatched());
    QCOMPARE(QFileInfo(tree.root() + QLatin1Char('/') + node).lastModified(), before);
    QCOMPARE(tree.read(node), QStringLiteral("40"));
}

void ActuatorTest::detectsAValueItDidNotWrite()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    Actuator actuator(sysfs);
    actuator.resolve();
    actuator.apply(quietPoints());
    QVERIFY(!actuator.lastApplyMismatched());

    // Another process wrote the node and left it writable, which is exactly
    // what a second power daemon does. The value is restored, and the
    // ownership violation is recorded rather than silently undone.
    const QString node = FakeSysfs::raplPackage()
            + QStringLiteral("/constraint_0_power_limit_uw");
    tree.write(node, QStringLiteral("200000000"));
    actuator.apply(quietPoints());
    QVERIFY(actuator.lastApplyMismatched());
    QCOMPARE(tree.read(node), QStringLiteral("12000000"));

    // A setpoint change of the daemon's own is not a foreign write.
    Setpoints other = quietPoints();
    other.pl1W = 20;
    actuator.apply(other);
    QVERIFY(!actuator.lastApplyMismatched());
}

void ActuatorTest::detectsANodeThatDisappeared()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    Actuator actuator(sysfs);
    actuator.resolve();
    actuator.apply(quietPoints());
    QVERIFY(!actuator.nodesVanished());

    // A hwmon device that re-registers within a boot moves; writing into the
    // stale path would fail silently for the rest of the boot.
    tree.remove(FakeSysfs::asus() + QStringLiteral("/pwm1_enable"));
    Setpoints emergency = quietPoints();
    emergency.fanMode = 0;
    actuator.apply(emergency);
    QVERIFY(actuator.nodesVanished());
    QVERIFY(actuator.lastApplyMismatched());
}

void ActuatorTest::refusesTheLazyCurveWithoutAPowerLimit()
{
    FakeSysfs tree;
    tree.remove(FakeSysfs::raplPackage() + QStringLiteral("/name"));
    Sysfs sysfs(tree.root());
    Actuator actuator(sysfs);
    actuator.resolve();

    // Without a power limit the quiet curve would leave the firmware's own
    // limit standing while the EC tolerates more heat before it ramps: hotter
    // than either endpoint.
    actuator.apply(quietPoints());
    QCOMPARE(tree.read(FakeSysfs::platformProfile()), QStringLiteral("balanced"));
    QVERIFY(actuator.hasAnyControl());
}

void ActuatorTest::reportsATreeItCannotManage()
{
    QTemporaryDir bare;
    Sysfs sysfs(bare.path());
    Actuator actuator(sysfs);
    actuator.resolve();
    // Not a machine this daemon can manage at all, which is a different thing
    // from a machine missing one control.
    QVERIFY(!actuator.hasAnyControl());
    actuator.apply(quietPoints());
    actuator.restore();
}

void ActuatorTest::missingNodesAreReportedNotFatal()
{
    FakeSysfs tree;
    tree.remove(FakeSysfs::platformProfile());
    tree.remove(FakeSysfs::asus() + QStringLiteral("/pwm1_enable"));
    tree.remove(FakeSysfs::card() + QStringLiteral("/gt_max_freq_mhz"));
    Sysfs sysfs(tree.root());
    Actuator actuator(sysfs);
    actuator.resolve();

    QVERIFY(!actuator.missingNodes().isEmpty());
    // Everything that does exist is still applied.
    actuator.apply(quietPoints());
    QCOMPARE(tree.read(FakeSysfs::pstate() + QStringLiteral("/max_perf_pct")),
             QStringLiteral("40"));
}

void ActuatorTest::restorePutsTheFanBackOnEveryPath()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    Actuator actuator(sysfs);
    actuator.resolve();

    Setpoints emergency = quietPoints();
    emergency.fanMode = 0;
    actuator.apply(emergency);
    QCOMPARE(tree.read(FakeSysfs::asus() + QStringLiteral("/pwm1_enable")), QStringLiteral("0"));

    actuator.restore();
    QCOMPARE(tree.read(FakeSysfs::asus() + QStringLiteral("/pwm1_enable")), QStringLiteral("2"));
}

void ActuatorTest::restoreNeverRaisesPastTheConfirmedCeiling()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    Actuator actuator(sysfs);
    actuator.resolve();
    actuator.restore();

    // Not the firmware's 200 W: returning to a "normal" state is not a licence
    // to raise a limit above what has been confirmed on this machine.
    QCOMPARE(tree.read(FakeSysfs::raplPackage() + QStringLiteral("/constraint_0_power_limit_uw")),
             QString::number(qint64(kMaxConfirmedPl1W) * 1000000));
    QCOMPARE(tree.read(FakeSysfs::raplPackage() + QStringLiteral("/constraint_1_power_limit_uw")),
             QString::number(qint64(kMaxConfirmedPl2W) * 1000000));
}

QTEST_MAIN(ActuatorTest)
#include "actuator_test.moc"
