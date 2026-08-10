// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "backlight.h"
#include "fake_sysfs.h"
#include "light_sensor.h"

#include <QTest>

class BacklightTest : public QObject {
    Q_OBJECT

private slots:
    void init();

    void thePanelIsFoundByItsScaleRatherThanItsName();
    void percentagesConvertBothWays();
    void writesAreClampedToThePanelScale();
    void theMemoryOfTheLastLevelIsWhatDetectsSomeoneElse();
    void anAbsentPanelLeavesTheObjectInert();
    void aPanelWithoutAScaleIsNotUsable();
    void theSensorIsFoundByItsReadingRatherThanItsIndex();
    void anAbsentSensorReadsAsNothing();

private:
    std::unique_ptr<FakeSysfs> m_tree;
};

void BacklightTest::init()
{
    m_tree = std::make_unique<FakeSysfs>();
}

void BacklightTest::thePanelIsFoundByItsScaleRatherThanItsName()
{
    Sysfs sysfs(m_tree->root());
    Backlight backlight(sysfs);
    backlight.resolve();

    QVERIFY(backlight.available());
    QCOMPARE(backlight.maxRaw(), FakeSysfs::kBacklightMax);
    QCOMPARE(backlight.readRaw().value_or(-1), FakeSysfs::kBacklightMax / 2);
    QCOMPARE(backlight.readPercent().value_or(-1), 50);
}

void BacklightTest::percentagesConvertBothWays()
{
    Sysfs sysfs(m_tree->root());
    Backlight backlight(sysfs);
    backlight.resolve();

    QCOMPARE(backlight.toRaw(0), 0);
    QCOMPARE(backlight.toRaw(100), FakeSysfs::kBacklightMax);
    QCOMPARE(backlight.toRaw(25), FakeSysfs::kBacklightMax / 4);
    QCOMPARE(backlight.toPercent(FakeSysfs::kBacklightMax), 100);
    QCOMPARE(backlight.toPercent(0), 0);
    // Out of range in either direction is the panel's own limit, not an error:
    // the caller works in percent and cannot know the scale.
    QCOMPARE(backlight.toRaw(500), FakeSysfs::kBacklightMax);
    QCOMPARE(backlight.toPercent(-20), 0);
}

void BacklightTest::writesAreClampedToThePanelScale()
{
    Sysfs sysfs(m_tree->root());
    Backlight backlight(sysfs);
    backlight.resolve();

    QVERIFY(backlight.writeRaw(4000));
    QCOMPARE(m_tree->read(FakeSysfs::backlight() + QStringLiteral("/brightness")),
             QString::number(FakeSysfs::kBacklightMax));
    QVERIFY(backlight.writeRaw(-5));
    QCOMPARE(m_tree->read(FakeSysfs::backlight() + QStringLiteral("/brightness")),
             QStringLiteral("0"));
}

void BacklightTest::theMemoryOfTheLastLevelIsWhatDetectsSomeoneElse()
{
    Sysfs sysfs(m_tree->root());
    Backlight backlight(sysfs);
    backlight.resolve();

    // Nothing known until something is written or noted: before the first
    // write there is no level to compare a reading against.
    QVERIFY(!backlight.lastKnownRaw().has_value());

    QVERIFY(backlight.writeRaw(120));
    QCOMPARE(backlight.lastKnownRaw().value_or(-1), 120);
    QCOMPARE(backlight.readRaw().value_or(-1), 120);

    // Somebody else moved it. The reading and the memory disagree, and that
    // disagreement is the whole detection mechanism.
    m_tree->write(FakeSysfs::backlight() + QStringLiteral("/brightness"), QStringLiteral("300"));
    QCOMPARE(backlight.readRaw().value_or(-1), 300);
    QCOMPARE(backlight.lastKnownRaw().value_or(-1), 120);

    backlight.noteObserved(300);
    QCOMPARE(backlight.lastKnownRaw().value_or(-1), 300);
}

void BacklightTest::anAbsentPanelLeavesTheObjectInert()
{
    m_tree->remove(FakeSysfs::backlight() + QStringLiteral("/brightness"));
    m_tree->remove(FakeSysfs::backlight() + QStringLiteral("/max_brightness"));

    Sysfs sysfs(m_tree->root());
    Backlight backlight(sysfs);
    backlight.resolve();

    QVERIFY(!backlight.available());
    QVERIFY(!backlight.readRaw().has_value());
    QVERIFY(!backlight.readPercent().has_value());
    QVERIFY(!backlight.writeRaw(100));
    QCOMPARE(backlight.toPercent(200), 0);
}

void BacklightTest::aPanelWithoutAScaleIsNotUsable()
{
    // A panel whose scale has no top has no percentages either.
    m_tree->write(FakeSysfs::backlight() + QStringLiteral("/max_brightness"),
                  QStringLiteral("0"));

    Sysfs sysfs(m_tree->root());
    Backlight backlight(sysfs);
    backlight.resolve();
    QVERIFY(!backlight.available());
}

void BacklightTest::theSensorIsFoundByItsReadingRatherThanItsIndex()
{
    Sysfs sysfs(m_tree->root());
    LightSensor sensor(sysfs);
    sensor.resolve();

    // iio:device1 comes first and has no illuminance channel, so anything that
    // takes the first directory it sees fails here.
    QVERIFY(sensor.available());
    QCOMPARE(sensor.devicePath(), FakeSysfs::als());
    // Millilux: the fake tree publishes the scale this machine has, 0.001, so
    // a raw count of 1000 is one lux.
    QCOMPARE(sensor.read().value_or(0), 1000u);

    // Nothing is clamped on the way out. A sixteen-bit ceiling would sit at
    // 65 lux, below an ordinary room with the ceiling light on, which measures
    // 103, and every brighter room would read as the same number.
    m_tree->write(FakeSysfs::als() + QStringLiteral("/in_illuminance_raw"),
                  QStringLiteral("999999"));
    QCOMPARE(sensor.read().value_or(0), 999999u);

    // The scale is read rather than assumed. A driver publishing whole lux
    // gives a thousand times the millilux for the same count.
    m_tree->write(FakeSysfs::als() + QStringLiteral("/in_illuminance_scale"),
                  QStringLiteral("1.000000"));
    sensor.resolve();
    QCOMPARE(sensor.read().value_or(0), 999999000u);

    // A driver publishing neither scale nor offset means the raw count is
    // already the unit.
    m_tree->remove(FakeSysfs::als() + QStringLiteral("/in_illuminance_scale"));
    m_tree->write(FakeSysfs::als() + QStringLiteral("/in_illuminance_raw"),
                  QStringLiteral("7"));
    sensor.resolve();
    QCOMPARE(sensor.read().value_or(0), 7000u);

    // Negative is not a light level.
    m_tree->write(FakeSysfs::als() + QStringLiteral("/in_illuminance_raw"),
                  QStringLiteral("-3"));
    QCOMPARE(sensor.read().value_or(1), 0u);
}

void BacklightTest::anAbsentSensorReadsAsNothing()
{
    m_tree->remove(FakeSysfs::als() + QStringLiteral("/in_illuminance_raw"));

    Sysfs sysfs(m_tree->root());
    LightSensor sensor(sysfs);
    sensor.resolve();
    QVERIFY(!sensor.available());
    QVERIFY(!sensor.read().has_value());

    // A node that is there but unreadable is absence too, not zero light.
    LightSensor other(sysfs);
    m_tree->write(FakeSysfs::als() + QStringLiteral("/in_illuminance_raw"),
                  QStringLiteral("not a number"));
    other.resolve();
    QVERIFY(other.available());
    QVERIFY(!other.read().has_value());
}

QTEST_MAIN(BacklightTest)
#include "backlight_test.moc"
