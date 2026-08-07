// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "daemon_state.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class DaemonStateTest : public QObject {
    Q_OBJECT

private slots:
    void everyKeyMakesTheRoundTrip();
    void anAbsentFileYieldsTheDefaults();
    void unknownKeysAndBrokenValuesAreReportedAndSkipped();
    void unsetValuesAreLeftOutOfTheFile();
    void anUnwritableFileIsReportedRatherThanThrown();

private:
    QString path() const { return m_work.path() + QStringLiteral("/state"); }
    void writeState(const QString &contents) const;

    QTemporaryDir m_work;
};

void DaemonStateTest::writeState(const QString &contents) const
{
    QFile file(path());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(contents.toUtf8());
}

void DaemonStateTest::everyKeyMakesTheRoundTrip()
{
    DaemonState state;
    state.mode = QStringLiteral("quiet");
    state.autoBrightness = true;
    state.brightnessProfile = QStringLiteral("evening");
    state.brightnessBase = 72;
    state.chargeLimit = 80;
    QVERIFY(state.save(path()));

    const DaemonState reloaded = DaemonState::load(path());
    QCOMPARE(reloaded.mode, QStringLiteral("quiet"));
    QCOMPARE(reloaded.autoBrightness, true);
    QCOMPARE(reloaded.brightnessProfile, QStringLiteral("evening"));
    QCOMPARE(reloaded.brightnessBase, 72);
    QCOMPARE(reloaded.chargeLimit, 80);
}

void DaemonStateTest::anAbsentFileYieldsTheDefaults()
{
    QFile::remove(path());
    QStringList problems;
    const DaemonState state = DaemonState::load(path(), &problems);

    // A first boot has no state file, so that is not a problem to report.
    QVERIFY(problems.isEmpty());
    QVERIFY(state.mode.isEmpty());
    // Off by default: the automation is something the person switches on.
    QCOMPARE(state.autoBrightness, false);
    QCOMPARE(state.brightnessBase, DaemonState::kBrightnessBaseUnset);
    QCOMPARE(state.chargeLimit, DaemonState::kChargeLimitUnset);
}

void DaemonStateTest::unknownKeysAndBrokenValuesAreReportedAndSkipped()
{
    writeState(QStringLiteral("# a comment\n"
                              "\n"
                              "mode=quiet\n"
                              "auto_brightness=perhaps\n"
                              "brightness_base=250\n"
                              "brightness_base_typo\n"
                              "charge_limit=lots\n"
                              "future_key=42\n"));

    QStringList problems;
    const DaemonState state = DaemonState::load(path(), &problems);

    // One typo must not leave the machine without a mode.
    QCOMPARE(state.mode, QStringLiteral("quiet"));
    QCOMPARE(state.autoBrightness, false);
    QCOMPARE(state.brightnessBase, DaemonState::kBrightnessBaseUnset);
    QCOMPARE(state.chargeLimit, DaemonState::kChargeLimitUnset);
    QCOMPARE(problems.size(), 5);
}

void DaemonStateTest::unsetValuesAreLeftOutOfTheFile()
{
    DaemonState state;
    state.mode = QStringLiteral("work");
    state.brightnessProfile = QStringLiteral("indoors");
    QVERIFY(state.save(path()));

    QFile file(path());
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QString::fromUtf8(file.readAll());
    QVERIFY(text.contains(QStringLiteral("mode=work")));
    QVERIFY(text.contains(QStringLiteral("auto_brightness=off")));
    // A threshold nobody set is not a threshold of zero, and a base nobody set
    // is taken from the panel rather than invented.
    QVERIFY(!text.contains(QStringLiteral("charge_limit")));
    QVERIFY(!text.contains(QStringLiteral("brightness_base")));
}

void DaemonStateTest::anUnwritableFileIsReportedRatherThanThrown()
{
    const QString unwritable = QStringLiteral("/proc/self/impossible/state");
    DaemonState state;
    state.mode = QStringLiteral("work");
    QVERIFY(!state.save(unwritable));

    QStringList problems;
    const DaemonState loaded = DaemonState::load(unwritable, &problems);
    QVERIFY(loaded.mode.isEmpty());
}

QTEST_MAIN(DaemonStateTest)
#include "daemon_state_test.moc"
