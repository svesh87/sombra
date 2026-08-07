// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "fake_sysfs.h"
#include "power_source.h"

#include <QTest>

class PowerSourceTest : public QObject {
    Q_OBJECT

private slots:
    void readsMainsState();
    void readsAndWritesTheChargeThreshold();
    void clampsTheChargeThreshold();
    void absentControlIsReportedNotAssumed();
};

void PowerSourceTest::readsMainsState()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    PowerSource source(sysfs);
    source.resolve();

    QVERIFY(source.onAc());
    tree.write(QStringLiteral("sys/class/power_supply/AC0/online"), QStringLiteral("0"));
    QVERIFY(!source.onAc());
}

void PowerSourceTest::readsAndWritesTheChargeThreshold()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    PowerSource source(sysfs);
    source.resolve();

    QVERIFY(source.hasChargeControl());
    QCOMPARE(source.chargeLimit().value_or(0), 100);
    QVERIFY(source.setChargeLimit(80));
    QCOMPARE(source.chargeLimit().value_or(0), 80);
    QCOMPARE(tree.read(QStringLiteral("sys/class/power_supply/BAT0/charge_control_end_threshold")),
             QStringLiteral("80"));
}

void PowerSourceTest::clampsTheChargeThreshold()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    PowerSource source(sysfs);
    source.resolve();

    QVERIFY(source.setChargeLimit(500));
    QCOMPARE(source.chargeLimit().value_or(0), 100);
    QVERIFY(source.setChargeLimit(-10));
    QCOMPARE(source.chargeLimit().value_or(0), 20);
}

void PowerSourceTest::absentControlIsReportedNotAssumed()
{
    FakeSysfs tree;
    tree.remove(QStringLiteral("sys/class/power_supply/BAT0/charge_control_end_threshold"));
    tree.remove(QStringLiteral("sys/class/power_supply/AC0/online"));
    Sysfs sysfs(tree.root());
    PowerSource source(sysfs);
    source.resolve();

    QVERIFY(!source.hasChargeControl());
    QVERIFY(!source.chargeLimit().has_value());
    QVERIFY(!source.setChargeLimit(80));
    // No mains node means the answer is battery, which is the state with the
    // smaller thermal budget.
    QVERIFY(!source.onAc());
}

QTEST_MAIN(PowerSourceTest)
#include "power_source_test.moc"
