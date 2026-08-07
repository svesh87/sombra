// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "fake_sysfs.h"
#include "sysfs.h"

#include <QTest>

#include <unistd.h>

class SysfsTest : public QObject {
    Q_OBJECT

private slots:
    void resolvesHwmonByNameNotByIndex();
    void resolvesRaplDomainByName();
    void missingNodeIsAbsentNotFatal();
    void malformedNumberIsRejected();
    void writeChecksExistenceFirst();
    void writeFailsOnUnwritableNode();
    void globIsEmptyForMissingDirectory();
    void rootIsNormalised();
};

void SysfsTest::resolvesHwmonByNameNotByIndex()
{
    FakeSysfs tree;
    QVERIFY(tree.isValid());
    Sysfs sysfs(tree.root());

    const auto coretemp = sysfs.hwmonByName(QStringLiteral("coretemp"));
    QVERIFY(coretemp.has_value());
    QCOMPARE(*coretemp, FakeSysfs::coretemp());

    const auto asus = sysfs.hwmonByName(QStringLiteral("asus"));
    QVERIFY(asus.has_value());
    QCOMPARE(*asus, FakeSysfs::asus());

    QVERIFY(!sysfs.hwmonByName(QStringLiteral("nothing-like-this")).has_value());
}

void SysfsTest::resolvesRaplDomainByName()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    QCOMPARE(sysfs.raplDomainByName(QStringLiteral("package-0")).value_or(QString()),
             FakeSysfs::raplPackage());
    QCOMPARE(sysfs.raplDomainByName(QStringLiteral("uncore")).value_or(QString()),
             FakeSysfs::raplUncore());
    QVERIFY(!sysfs.raplDomainByName(QStringLiteral("psys")).has_value());
}

void SysfsTest::missingNodeIsAbsentNotFatal()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    QVERIFY(!sysfs.exists(QStringLiteral("sys/class/hwmon/hwmon99/name")));
    QVERIFY(!sysfs.readText(QStringLiteral("sys/class/hwmon/hwmon99/name")).has_value());
    QVERIFY(!sysfs.readNumber(QStringLiteral("sys/class/hwmon/hwmon99/name")).has_value());
}

void SysfsTest::malformedNumberIsRejected()
{
    FakeSysfs tree;
    tree.write(FakeSysfs::asus() + QStringLiteral("/fan1_input"), QStringLiteral("not a number"));
    Sysfs sysfs(tree.root());
    QVERIFY(!sysfs.readNumber(FakeSysfs::asus() + QStringLiteral("/fan1_input")).has_value());
    QCOMPARE(sysfs.readText(FakeSysfs::asus() + QStringLiteral("/fan1_input")).value_or(QString()),
             QStringLiteral("not a number"));
}

void SysfsTest::writeChecksExistenceFirst()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    QVERIFY(!sysfs.writeNumber(QStringLiteral("sys/does/not/exist"), 1));
    QVERIFY(!QFile::exists(sysfs.absolute(QStringLiteral("sys/does/not/exist"))));

    QVERIFY(sysfs.writeNumber(FakeSysfs::pstate() + QStringLiteral("/max_perf_pct"), 40));
    QCOMPARE(tree.read(FakeSysfs::pstate() + QStringLiteral("/max_perf_pct")),
             QStringLiteral("40"));
}

void SysfsTest::writeFailsOnUnwritableNode()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    const QString node = FakeSysfs::pstate() + QStringLiteral("/no_turbo");
    tree.makeUnwritable(node);
    // Root may write a read-only file, so the assertion is on the outcome
    // that holds either way: the node is still readable and the daemon does
    // not treat a refused write as fatal.
    const bool written = sysfs.writeNumber(node, 1);
    QCOMPARE(written, ::geteuid() == 0);
    QVERIFY(sysfs.readNumber(node).has_value());
}

void SysfsTest::globIsEmptyForMissingDirectory()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    QVERIFY(sysfs.glob(QStringLiteral("sys/class/nothing"), QStringLiteral("*")).isEmpty());
    QCOMPARE(sysfs.glob(QStringLiteral("sys/class/drm"), QStringLiteral("card*")).size(), 1);
}

void SysfsTest::rootIsNormalised()
{
    FakeSysfs tree;
    Sysfs trailing(tree.root() + QStringLiteral("/"));
    QVERIFY(trailing.exists(QStringLiteral("/") + FakeSysfs::platformProfile()));
    QCOMPARE(Sysfs(QString()).root(), QStringLiteral("/"));
}

QTEST_MAIN(SysfsTest)
#include "sysfs_test.moc"
