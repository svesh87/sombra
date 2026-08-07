// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "modes.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class ModesTest : public QObject {
    Q_OBJECT

private slots:
    void builtinTableIsUsableOnItsOwn();
    void parsesAConfigurationFile();
    void clampsAboveTheConfirmedCeiling();
    void reportsUnknownEnumValuesAndFallsBack();
    void missingFileLeavesTheTableAlone();
    void fileWithoutModesIsRejected();
    void unknownDefaultFallsBackToTheMiddleMode();
    void tableIsOrderedByPowerNotByFileOrder();
    void mostConservativeIsTheLowestPowerLimit();
    void snippetRoundTripsThroughTheParser();

private:
    QString writeConfig(const QString &contents);

    QTemporaryDir m_dir;
    int m_counter = 0;
};

QString ModesTest::writeConfig(const QString &contents)
{
    const QString path = m_dir.path() + QStringLiteral("/modes%1.conf").arg(++m_counter);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(contents.toUtf8());
    return path;
}

void ModesTest::builtinTableIsUsableOnItsOwn()
{
    const ModeTable table = ModeTable::builtinDefaults();
    QCOMPARE(table.modes().size(), 3);
    // The middle mode is the working one and is what the machine boots into.
    QCOMPARE(table.defaultModeId(), QStringLiteral("work"));
    QCOMPARE(table.indexOf(QStringLiteral("work")), 1);
    QCOMPARE(table.indexOf(QStringLiteral("absent")), -1);
    QVERIFY(table.find(QStringLiteral("quiet")));
    QVERIFY(!table.find(QStringLiteral("absent")));

    for (const Mode &mode : table.modes()) {
        QVERIFY2(mode.setpoints.pl1W <= kMaxConfirmedPl1W, qPrintable(mode.id));
        QVERIFY2(mode.setpoints.pl2W <= kMaxConfirmedPl2W, qPrintable(mode.id));
        // No mode may ask for the fan; that is explicit-command territory.
        QCOMPARE(mode.setpoints.fanMode, 2);
    }
}

void ModesTest::parsesAConfigurationFile()
{
    const QString path = writeConfig(QStringLiteral("# a comment survives, the daemon only reads\n"
                                                    "[general]\n"
                                                    "default_mode=silent\n"
                                                    "\n"
                                                    "[mode silent]\n"
                                                    "label=Silent\n"
                                                    "platform_profile=quiet\n"
                                                    "pl1_w=9\n"
                                                    "pl2_w=14\n"
                                                    "max_perf_pct=25\n"
                                                    "turbo=off\n"
                                                    "epp=power\n"
                                                    "hwp_dynamic_boost=0\n"
                                                    "gt_max_mhz=800\n"
                                                    "\n"
                                                    "[mode loud]\n"
                                                    "label=Loud\n"
                                                    "platform_profile=performance\n"
                                                    "pl1_w=45\n"
                                                    "pl2_w=65\n"
                                                    "max_perf_pct=100\n"
                                                    "turbo=on\n"
                                                    "epp=balance_performance\n"
                                                    "hwp_dynamic_boost=1\n"
                                                    "gt_max_mhz=2350\n"));

    ModeTable table;
    QStringList problems;
    QVERIFY(table.load(path, &problems));
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
    QCOMPARE(table.modes().size(), 2);
    QCOMPARE(table.defaultModeId(), QStringLiteral("silent"));

    const Mode *silent = table.find(QStringLiteral("silent"));
    QVERIFY(silent);
    QCOMPARE(silent->label, QStringLiteral("Silent"));
    QCOMPARE(silent->setpoints.profile, Profile::Quiet);
    QCOMPARE(silent->setpoints.pl1W, 9);
    QCOMPARE(silent->setpoints.pl2W, 14);
    QCOMPARE(silent->setpoints.maxPerfPct, 25);
    QVERIFY(silent->setpoints.noTurbo);
    QCOMPARE(silent->setpoints.epp, Epp::Power);
    QVERIFY(!silent->setpoints.hwpBoost);
    QCOMPARE(silent->setpoints.gtMaxMhz, 800);

    const Mode *loud = table.find(QStringLiteral("loud"));
    QVERIFY(loud);
    QVERIFY(!loud->setpoints.noTurbo);
    QVERIFY(loud->setpoints.hwpBoost);
    QCOMPARE(loud->setpoints.profile, Profile::Performance);
}

void ModesTest::clampsAboveTheConfirmedCeiling()
{
    // The firmware's own 200 W is a fault condition. Nothing in a text file
    // may talk the daemon into reproducing it.
    const QString path = writeConfig(QStringLiteral("[mode greedy]\n"
                                                    "pl1_w=200\n"
                                                    "pl2_w=250\n"
                                                    "max_perf_pct=400\n"
                                                    "gt_max_mhz=9000\n"));
    ModeTable table;
    QStringList problems;
    QVERIFY(table.load(path, &problems));
    const Mode *greedy = table.find(QStringLiteral("greedy"));
    QVERIFY(greedy);
    QCOMPARE(greedy->setpoints.pl1W, kMaxConfirmedPl1W);
    QCOMPARE(greedy->setpoints.pl2W, kMaxConfirmedPl2W);
    QCOMPARE(greedy->setpoints.maxPerfPct, 100);
    QCOMPARE(greedy->setpoints.gtMaxMhz, kMaxGtMhz);
    // Clamping is reported, never silent.
    QCOMPARE(problems.size(), 4);

    const QString low = writeConfig(QStringLiteral("[mode tiny]\n"
                                                   "pl1_w=0\n"
                                                   "max_perf_pct=1\n"
                                                   "gt_max_mhz=10\n"));
    ModeTable second;
    QStringList moreProblems;
    QVERIFY(second.load(low, &moreProblems));
    const Mode *tiny = second.find(QStringLiteral("tiny"));
    QVERIFY(tiny);
    QCOMPARE(tiny->setpoints.pl1W, 1);
    QCOMPARE(tiny->setpoints.maxPerfPct, kMinPerfPct);
    QCOMPARE(tiny->setpoints.gtMaxMhz, kMinGtMhz);
}

void ModesTest::reportsUnknownEnumValuesAndFallsBack()
{
    const QString path = writeConfig(QStringLiteral("[mode odd]\n"
                                                    "platform_profile=turbo-max\n"
                                                    "epp=whatever\n"
                                                    "turbo=maybe\n"));
    ModeTable table;
    QStringList problems;
    QVERIFY(table.load(path, &problems));
    const Mode *odd = table.find(QStringLiteral("odd"));
    QVERIFY(odd);
    QCOMPARE(odd->setpoints.profile, Profile::Balanced);
    QCOMPARE(odd->setpoints.epp, Epp::Power);
    // An unparsable boolean keeps the safer default rather than guessing.
    QVERIFY(odd->setpoints.noTurbo);
    QCOMPARE(problems.size(), 2);
}

void ModesTest::missingFileLeavesTheTableAlone()
{
    ModeTable table = ModeTable::builtinDefaults();
    QStringList problems;
    QVERIFY(!table.load(m_dir.path() + QStringLiteral("/absent.conf"), &problems));
    QCOMPARE(problems.size(), 1);
    QCOMPARE(table.modes().size(), 3);
}

void ModesTest::fileWithoutModesIsRejected()
{
    const QString path = writeConfig(QStringLiteral("[general]\ndefault_mode=work\n"));
    ModeTable table = ModeTable::builtinDefaults();
    QStringList problems;
    QVERIFY(!table.load(path, &problems));
    QCOMPARE(table.modes().size(), 3);

    const QString empty = writeConfig(QStringLiteral("[mode ]\nlabel=nameless\n"));
    ModeTable second = ModeTable::builtinDefaults();
    QVERIFY(!second.load(empty, &problems));
}

void ModesTest::unknownDefaultFallsBackToTheMiddleMode()
{
    const QString path = writeConfig(QStringLiteral("[general]\n"
                                                    "default_mode=nonexistent\n"
                                                    "[mode a]\npl1_w=10\n"
                                                    "[mode b]\npl1_w=20\n"
                                                    "[mode c]\npl1_w=30\n"));
    ModeTable table;
    QStringList problems;
    QVERIFY(table.load(path, &problems));
    QCOMPARE(table.defaultModeId(), QStringLiteral("b"));
    QVERIFY(problems.join(QStringLiteral(" ")).contains(QStringLiteral("nonexistent")));

    const QString silent = writeConfig(QStringLiteral("[mode a]\npl1_w=10\n[mode b]\npl1_w=20\n"
                                                      "[mode c]\npl1_w=30\n"));
    ModeTable second;
    QStringList none;
    QVERIFY(second.load(silent, &none));
    QCOMPARE(second.defaultModeId(), QStringLiteral("b"));
    QVERIFY(none.isEmpty());
}

void ModesTest::tableIsOrderedByPowerNotByFileOrder()
{
    // The table is a scale from the coolest envelope to the widest one. The
    // desktop panel's three slots map onto that scale, and a sample records
    // the mode by index, so the order has to come from the numbers rather than
    // from how the file happens to be written.
    const QString path = writeConfig(QStringLiteral("[mode zulu]\npl1_w=30\n"
                                                    "[mode alpha]\npl1_w=40\n"
                                                    "[mode mike]\npl1_w=15\n"));
    ModeTable table;
    QStringList problems;
    QVERIFY(table.load(path, &problems));
    QCOMPARE(table.modes().at(0).id, QStringLiteral("mike"));
    QCOMPARE(table.modes().at(1).id, QStringLiteral("zulu"));
    QCOMPARE(table.modes().at(2).id, QStringLiteral("alpha"));
    QCOMPARE(table.defaultModeId(), QStringLiteral("zulu"));

    // Equal limits fall back to the name, so the order is still total.
    const QString tied = writeConfig(QStringLiteral("[mode second]\npl1_w=20\n"
                                                    "[mode first]\npl1_w=20\n"));
    ModeTable ties;
    QVERIFY(ties.load(tied, &problems));
    QCOMPARE(ties.modes().at(0).id, QStringLiteral("first"));
    QCOMPARE(ties.modes().at(1).id, QStringLiteral("second"));
}

void ModesTest::mostConservativeIsTheLowestPowerLimit()
{
    const ModeTable table = ModeTable::builtinDefaults();
    const Mode *conservative = table.mostConservative();
    QVERIFY(conservative);
    QCOMPARE(conservative->id, QStringLiteral("quiet"));

    ModeTable empty;
    QVERIFY(!empty.mostConservative());
}

void ModesTest::snippetRoundTripsThroughTheParser()
{
    Setpoints tuned;
    tuned.profile = Profile::Quiet;
    tuned.pl1W = 14;
    tuned.pl2W = 22;
    tuned.maxPerfPct = 35;
    tuned.noTurbo = false;
    tuned.epp = Epp::BalancePower;
    tuned.hwpBoost = true;
    tuned.gtMaxMhz = 1200;

    const QString snippet = ModeTable::toConfigSnippet(QStringLiteral("tuned"), tuned);
    const QString path = writeConfig(snippet);

    ModeTable table;
    QStringList problems;
    QVERIFY(table.load(path, &problems));
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
    const Mode *mode = table.find(QStringLiteral("tuned"));
    QVERIFY(mode);
    QCOMPARE(mode->label, QStringLiteral("tuned"));
    QCOMPARE(mode->setpoints, tuned);
}

QTEST_MAIN(ModesTest)
#include "modes_test.moc"
