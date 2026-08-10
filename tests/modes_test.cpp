// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "modes.h"

#include "settings_file.h"

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
    void theSectionReadsBackAsTheSameMode();
    void theWholeFileReadsBackAsTheSameTables();
    void savingRefusesWhatItCannotWrite();
    void modesAreEditedNeverCreated();
    void oneSettingIsValidatedTheSameWayFromBothSides();

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
    // "default" is among them on purpose: the kernel lists it, so a plausible
    // configuration can ask for it, and it has to be refused with a reason
    // rather than written and then read back as something else.
    const QString path = writeConfig(QStringLiteral("[mode odd]\n"
                                                    "platform_profile=turbo-max\n"
                                                    "epp=default\n"
                                                    "turbo=maybe\n"));
    ModeTable table;
    QStringList problems;
    QVERIFY(table.load(path, &problems));
    const Mode *odd = table.find(QStringLiteral("odd"));
    QVERIFY(odd);
    QCOMPARE(odd->setpoints.profile, Profile::Balanced);
    QCOMPARE(odd->setpoints.epp, Epp::Power);
    // An unparsable boolean keeps the safer default rather than guessing, and
    // says so: all three bad values are reported, none of them silently.
    QVERIFY(odd->setpoints.noTurbo);
    QCOMPARE(problems.size(), 3);
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

void ModesTest::theSectionReadsBackAsTheSameMode()
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

    // The daemon writes this file and reads it back at the next boot, so a
    // section that does not survive the round trip loses settings silently.
    const QString section =
            ModeTable::toConfigSection(QStringLiteral("tuned"), QStringLiteral("Tuned"), tuned);
    const QString path = writeConfig(section);

    ModeTable table;
    QStringList problems;
    QVERIFY(table.load(path, &problems));
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
    const Mode *mode = table.find(QStringLiteral("tuned"));
    QVERIFY(mode);
    QCOMPARE(mode->label, QStringLiteral("Tuned"));
    QCOMPARE(mode->setpoints, tuned);
}

void ModesTest::theWholeFileReadsBackAsTheSameTables()
{
    ModeTable modes = ModeTable::builtinDefaults();
    Mode edited = *modes.find(QStringLiteral("work"));
    edited.label = QStringLiteral("Work, tuned");
    edited.setpoints.pl1W = 18;
    edited.setpoints.pl2W = 26;
    QVERIFY(modes.replace(edited));
    QVERIFY(modes.setDefaultModeId(QStringLiteral("quiet")));

    const BrightnessSettings brightness = builtinBrightnessSettings();
    const QString path = writeConfig(settings_file::serialize(modes, brightness));

    ModeTable readModes;
    QStringList problems;
    QVERIFY(readModes.load(path, &problems));
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
    QCOMPARE(readModes.modes().size(), modes.modes().size());
    QCOMPARE(readModes.defaultModeId(), QStringLiteral("quiet"));
    for (const Mode &mode : modes.modes()) {
        const Mode *read = readModes.find(mode.id);
        QVERIFY(read);
        QCOMPARE(read->label, mode.label);
        QCOMPARE(read->setpoints, mode.setpoints);
    }

    BrightnessSettings readBrightness;
    QVERIFY(loadBrightnessSettings(path, &readBrightness, &problems));
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
    QCOMPARE(readBrightness.responsePct, brightness.responsePct);
    QCOMPARE(readBrightness.floorPct, brightness.floorPct);
    QCOMPARE(readBrightness.curve.size(), brightness.curve.size());
    // Light levels are written in lux and read back as the same millilux, which
    // is the only place the two units meet.
    QCOMPARE(readBrightness.curve.last().light, brightness.curve.last().light);
}

void ModesTest::savingRefusesWhatItCannotWrite()
{
    // A regular file where the directory would have to be, so the write cannot
    // succeed. The caller has to be told rather than left believing a change
    // is durable when nothing was written.
    const QString blocker = m_dir.filePath(QStringLiteral("blocker"));
    QFile file(blocker);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    QString error;
    QVERIFY(!settings_file::save(blocker + QStringLiteral("/settings.conf"),
                                 ModeTable::builtinDefaults(), builtinBrightnessSettings(),
                                 &error));
    QVERIFY(!error.isEmpty());
}

void ModesTest::modesAreEditedNeverCreated()
{
    ModeTable table = ModeTable::builtinDefaults();
    Mode absent;
    absent.id = QStringLiteral("nonexistent");
    QVERIFY(!table.replace(absent));
    QCOMPARE(table.modes().size(), 3);
    QVERIFY(!table.setDefaultModeId(QStringLiteral("nonexistent")));
    QCOMPARE(table.defaultModeId(), QStringLiteral("work"));
}

void ModesTest::oneSettingIsValidatedTheSameWayFromBothSides()
{
    Mode mode = *ModeTable::builtinDefaults().find(QStringLiteral("work"));
    QString error;
    QStringList clamped;

    QVERIFY(!applyModeSetting(mode, QStringLiteral("pl1_w"), QStringLiteral("plenty"), &error,
                              &clamped));
    QVERIFY(error.contains(QStringLiteral("not a number")));
    QVERIFY(!applyModeSetting(mode, QStringLiteral("nonsense"), QStringLiteral("1"), &error,
                              &clamped));
    QVERIFY(error.contains(QStringLiteral("unknown setting")));

    // Out of range is clamped and reported rather than refused: one typo must
    // not leave the machine unmanaged.
    QVERIFY(applyModeSetting(mode, QStringLiteral("pl1_w"), QStringLiteral("500"), &error,
                             &clamped));
    QCOMPARE(mode.setpoints.pl1W, kMaxConfirmedPl1W);
    QCOMPARE(clamped.size(), 1);

    // PL2 below PL1 is what no single key can notice on its own.
    clamped.clear();
    QVERIFY(applyModeSetting(mode, QStringLiteral("pl2_w"), QStringLiteral("5"), &error, &clamped));
    normalizeMode(mode, &clamped);
    QCOMPARE(mode.setpoints.pl2W, mode.setpoints.pl1W);
    QCOMPARE(clamped.size(), 1);
    // No mode may drive the fan, whatever the file or the socket asks for.
    QCOMPARE(mode.setpoints.fanMode, 2);
}

QTEST_MAIN(ModesTest)
#include "modes_test.moc"
