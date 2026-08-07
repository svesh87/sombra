// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "profile_bridge.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class ProfileBridgeTest : public QObject {
    Q_OBJECT

private slots:
    void mapsTheThreeSlotsOntoTheModeScale();
    void balancedFollowsTheConfiguredDefault();
    void degradesForTablesThatAreNotThree();
    void unknownNamesMapToNothing();
    void standardNamesAreExactlyTheThree();

private:
    ModeTable tableFrom(const QString &contents);

    QTemporaryDir m_dir;
    int m_counter = 0;
};

ModeTable ProfileBridgeTest::tableFrom(const QString &contents)
{
    const QString path = m_dir.path() + QStringLiteral("/modes%1.conf").arg(++m_counter);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(contents.toUtf8());
    file.close();
    ModeTable table;
    table.load(path);
    return table;
}

void ProfileBridgeTest::mapsTheThreeSlotsOntoTheModeScale()
{
    const ModeTable table = ModeTable::builtinDefaults();
    QCOMPARE(profile_bridge::modeForProfile(table, QStringLiteral("power-saver")),
             QStringLiteral("quiet"));
    QCOMPARE(profile_bridge::modeForProfile(table, QStringLiteral("balanced")),
             QStringLiteral("work"));
    QCOMPARE(profile_bridge::modeForProfile(table, QStringLiteral("performance")),
             QStringLiteral("full"));

    QCOMPARE(profile_bridge::profileForMode(table, QStringLiteral("quiet")),
             QStringLiteral("power-saver"));
    QCOMPARE(profile_bridge::profileForMode(table, QStringLiteral("work")),
             QStringLiteral("balanced"));
    QCOMPARE(profile_bridge::profileForMode(table, QStringLiteral("full")),
             QStringLiteral("performance"));
}

void ProfileBridgeTest::balancedFollowsTheConfiguredDefault()
{
    // The middle slot follows the working mode rather than the arithmetic
    // middle: the working mode is the one the machine actually lives in.
    const ModeTable table = tableFrom(QStringLiteral("[general]\ndefault_mode=third\n"
                                                     "[mode first]\npl1_w=10\n"
                                                     "[mode second]\npl1_w=20\n"
                                                     "[mode third]\npl1_w=30\n"
                                                     "[mode fourth]\npl1_w=40\n"));
    QCOMPARE(table.defaultModeId(), QStringLiteral("third"));
    QCOMPARE(profile_bridge::modeForProfile(table, QStringLiteral("balanced")),
             QStringLiteral("third"));
    QCOMPARE(profile_bridge::modeForProfile(table, QStringLiteral("power-saver")),
             QStringLiteral("first"));
    QCOMPARE(profile_bridge::modeForProfile(table, QStringLiteral("performance")),
             QStringLiteral("fourth"));

    // A default that is one of the extremes leaves the middle slot on the
    // arithmetic middle, so the three slots stay distinct.
    const ModeTable edge = tableFrom(QStringLiteral("[general]\ndefault_mode=low\n"
                                                    "[mode low]\npl1_w=10\n"
                                                    "[mode mid]\npl1_w=20\n"
                                                    "[mode high]\npl1_w=30\n"));
    QCOMPARE(profile_bridge::modeForProfile(edge, QStringLiteral("balanced")),
             QStringLiteral("mid"));
}

void ProfileBridgeTest::degradesForTablesThatAreNotThree()
{
    const ModeTable single = tableFrom(QStringLiteral("[mode only]\npl1_w=20\n"));
    for (const QString &profile : profile_bridge::standardProfiles())
        QCOMPARE(profile_bridge::modeForProfile(single, profile), QStringLiteral("only"));
    QCOMPARE(profile_bridge::profileForMode(single, QStringLiteral("only")),
             QStringLiteral("balanced"));

    const ModeTable pair = tableFrom(QStringLiteral("[mode cool]\npl1_w=12\n"
                                                    "[mode warm]\npl1_w=30\n"));
    QCOMPARE(profile_bridge::modeForProfile(pair, QStringLiteral("power-saver")),
             QStringLiteral("cool"));
    QCOMPARE(profile_bridge::modeForProfile(pair, QStringLiteral("performance")),
             QStringLiteral("warm"));
    QCOMPARE(profile_bridge::profileForMode(pair, QStringLiteral("cool")),
             QStringLiteral("power-saver"));

    const ModeTable empty;
    QVERIFY(profile_bridge::modeForProfile(empty, QStringLiteral("balanced")).isEmpty());
}

void ProfileBridgeTest::unknownNamesMapToNothing()
{
    const ModeTable table = ModeTable::builtinDefaults();
    QVERIFY(profile_bridge::modeForProfile(table, QStringLiteral("turbo")).isEmpty());
    QVERIFY(profile_bridge::modeForProfile(table, QString()).isEmpty());
    QVERIFY(profile_bridge::profileForMode(table, QStringLiteral("absent")).isEmpty());
}

void ProfileBridgeTest::standardNamesAreExactlyTheThree()
{
    // The standard interface accepts these three and nothing else; the panel
    // has three slots and they are redefined as this project's modes.
    QCOMPARE(profile_bridge::standardProfiles(),
             QStringList({ QStringLiteral("power-saver"), QStringLiteral("balanced"),
                           QStringLiteral("performance") }));
}

QTEST_MAIN(ProfileBridgeTest)
#include "profile_bridge_test.moc"
