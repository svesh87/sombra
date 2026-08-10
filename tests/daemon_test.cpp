// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "daemon.h"
#include "exporter.h"
#include "fake_sysfs.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <memory>

namespace {

QJsonObject statusOf(Daemon &daemon)
{
    const QString reply = daemon.handleCommand(QStringLiteral("status"));
    return QJsonDocument::fromJson(reply.mid(3).toUtf8()).object();
}

} // namespace

class DaemonTest : public QObject {
    Q_OBJECT

private slots:
    void init();

    void appliesTheDefaultModeAtStart();
    void refusesATreeItCannotManage();
    void refusesToBecomeASecondOwner();
    void keepsManagingWithoutTelemetryStorage();
    void tickRecordsReadingsAndSetpointsTogether();
    void modeSelectionPersistsAndIsRestored();
    void unknownModeIsRefused();
    void liveTuningAppliesImmediatelyAndIsNotPersisted();
    void selectingAModeCancelsLiveTuning();
    void liveTuningRefusesNonsense();
    void liveTuningCannotExceedConfirmedLimits();
    void guardOverridesTheModeAndIsRecorded();
    void missingSensorsFallBackToTheMostConservativeMode();
    void marksAreStampedOntoSamples();
    void fanMaxIsExplicitOnly();
    void chargeThresholdIsSettable();
    void configurationIsRereadOnCommand();
    void unknownCommandsAreRejected();
    void shutdownRestoresTheHardware();
    void runsOverTheControlSocket();
    void stopsAfterTheRequestedNumberOfTicks();

    void theChargeThresholdIsReappliedAtStart();
    void theBacklightIsLeftAloneUntilTheAutomationIsSwitchedOn();
    void switchingTheAutomationOnOrOffDoesNotMoveTheBacklight();
    void theBacklightFollowsTheLightOnceItHasChanged();
    void theFadeMovesInSeveralStepsRatherThanOne();
    void aHandCorrectionBecomesTheNewBase();
    void theFloorBindsEverybodyIncludingThePerson();
    void theCeilingBindsEverybodyTheSameWay();
    void telemetryDepthIsASettingAndSwitchingItOffKeepsTheFile();
    void selectingAModeDoesNotMoveTheBacklight();
    void brightnessChoicesSurviveARestart();
    void brightnessCommandsAreValidated();
    void theBrightnessWindowIsTunedLive();
    void theBacklightSettingsAreDurable();
    void theBacklightPathDegradesOnItsOwn();
    void configShowAnswersWithWhatIsSaved();
    void theBaselineIsSetOutrightAndSurvivesARestart();
    void editingAModeIsDurableAndAppliesAtOnce();
    void modesAreEditedNotCreated();
    void aRefusedEditChangesNothingOnDiskOrInMemory();
    void liveTuningIsSavedIntoTheModeThatIsNamed();
    void resettingReturnsToTheBuiltInValues();
    void theDisplayCountIsRecorded();

private:
    DaemonOptions options() const;
    QString writeConfig(const QString &contents) const;

    int backlightRaw() const;
    void setBacklightByHand(int raw) const;
    void setLight(int value) const;
    // Runs the fade to completion. In service this is a 40 ms timer; here it
    // is stepped directly, which is why Daemon exposes it.
    void finishFade(Daemon &daemon) const;
    // Ticks until the daemon decides to move, and returns how many ticks that
    // took, or -1 if it never did.
    int tickUntilMove(Daemon &daemon, int limit) const;

    QTemporaryDir m_work;
    // Rebuilt before every test: several cases delete nodes or drive the
    // temperature up, and a shared tree would carry that into the next case.
    std::unique_ptr<FakeSysfs> m_tree;
};

void DaemonTest::init()
{
    m_tree = std::make_unique<FakeSysfs>();
    QFile::remove(m_work.path() + QStringLiteral("/state"));
    QFile::remove(m_work.path() + QStringLiteral("/metrics.ring"));
    QFile::remove(m_work.path() + QStringLiteral("/settings.conf"));
}

DaemonOptions DaemonTest::options() const
{
    DaemonOptions o;
    o.sysfsRoot = m_tree->root();
    o.configPath = m_work.path() + QStringLiteral("/settings.conf");
    o.statePath = m_work.path() + QStringLiteral("/state");
    o.ringPath = m_work.path() + QStringLiteral("/metrics.ring");
    o.socketPath = m_work.path() + QStringLiteral("/control.sock");
    o.ringCapacity = 64;
    o.flushSeconds = 2;
    return o;
}

QString DaemonTest::writeConfig(const QString &contents) const
{
    const QString path = m_work.path() + QStringLiteral("/settings.conf");
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(contents.toUtf8());
    return path;
}

int DaemonTest::backlightRaw() const
{
    return m_tree->read(FakeSysfs::backlight() + QStringLiteral("/brightness")).toInt();
}

void DaemonTest::setBacklightByHand(int raw) const
{
    m_tree->write(FakeSysfs::backlight() + QStringLiteral("/brightness"), QString::number(raw));
}

void DaemonTest::setLight(int value) const
{
    m_tree->write(FakeSysfs::als() + QStringLiteral("/in_illuminance_raw"),
                  QString::number(value));
}

void DaemonTest::finishFade(Daemon &daemon) const
{
    for (int i = 0; i < 1000 && daemon.fading(); ++i)
        daemon.fadeBrightness();
}

int DaemonTest::tickUntilMove(Daemon &daemon, int limit) const
{
    for (int i = 1; i <= limit; ++i) {
        daemon.tick();
        if (daemon.fading())
            return i;
    }
    return -1;
}

void DaemonTest::appliesTheDefaultModeAtStart()
{
    Daemon daemon(options());
    QString error;
    QVERIFY2(daemon.start(&error), qPrintable(error));

    // The middle mode is the working one and is what the machine boots into.
    QCOMPARE(daemon.activeModeId(), QStringLiteral("work"));
    QCOMPARE(m_tree->read(FakeSysfs::platformProfile()), QStringLiteral("balanced"));
    QCOMPARE(m_tree->read(FakeSysfs::raplPackage()
                         + QStringLiteral("/constraint_0_power_limit_uw")),
             QStringLiteral("20000000"));
    // The firmware's own 200 W is gone the moment the daemon takes over.
    QCOMPARE(m_tree->read(FakeSysfs::pstate() + QStringLiteral("/max_perf_pct")),
             QStringLiteral("50"));
    QCOMPARE(m_tree->read(FakeSysfs::pstate() + QStringLiteral("/no_turbo")), QStringLiteral("1"));
    // Automatic control never touches the fan.
    QCOMPARE(m_tree->read(FakeSysfs::asus() + QStringLiteral("/pwm1_enable")), QStringLiteral("2"));
}

void DaemonTest::refusesATreeItCannotManage()
{
    QTemporaryDir bare;
    DaemonOptions o = options();
    o.sysfsRoot = bare.path();
    Daemon daemon(o);
    QString error;
    QVERIFY(!daemon.start(&error));
    QVERIFY(error.contains(QStringLiteral("no thermal control nodes")));
}

void DaemonTest::refusesToBecomeASecondOwner()
{
    Daemon first(options());
    QVERIFY(first.start());

    // Two processes writing the same nodes produce race-dependent behaviour
    // rather than redundancy, and they would interleave two sample streams
    // into one ring. A live socket is the evidence, and the only startup
    // condition worth refusing over.
    DaemonOptions second = options();
    second.ringPath = m_work.path() + QStringLiteral("/second.ring");
    Daemon intruder(second);
    QString error;
    QVERIFY(!intruder.start(&error));
    QVERIFY(error.contains(QStringLiteral("another daemon")));
}

void DaemonTest::keepsManagingWithoutTelemetryStorage()
{
    DaemonOptions o = options();
    // A ring that cannot be created must not stop the machine being managed:
    // the log is secondary to the thermal envelope.
    o.ringPath = QStringLiteral("/proc/self/impossible/metrics.ring");
    Daemon daemon(o);
    QString error;
    QVERIFY2(daemon.start(&error), qPrintable(error));
    daemon.tick();
    QCOMPARE(m_tree->read(FakeSysfs::raplPackage()
                          + QStringLiteral("/constraint_0_power_limit_uw")),
             QStringLiteral("20000000"));
    QCOMPARE(daemon.tickCount(), quint64(1));
    QVERIFY(daemon.handleCommand(QStringLiteral("mark good")).startsWith(QStringLiteral("ok")));
}

void DaemonTest::tickRecordsReadingsAndSetpointsTogether()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    daemon.tick();
    daemon.tick();

    const Sample &s = daemon.lastSample();
    QCOMPARE(s.pkgTemp, int16_t(610));
    QCOMPARE(s.fanRpm, uint16_t(1900));
    // Every live setpoint travels with the readings; a log that does not say
    // what was applied cannot be analysed.
    QCOMPARE(s.pl1W, uint8_t(20));
    QCOMPARE(s.pl2W, uint8_t(30));
    QCOMPARE(s.maxPerfPct, uint8_t(50));
    QCOMPARE(s.noTurbo, uint8_t(1));
    QCOMPARE(s.epp, uint8_t(Epp::Power));
    QCOMPARE(s.profile, uint8_t(Profile::Balanced));
    QCOMPARE(s.modeId, uint8_t(1));
    QCOMPARE(s.chargeLimit, uint8_t(100));
    QVERIFY(s.flags & FlagOnAc);
    QVERIFY(!(s.flags & FlagGuardEngaged));
    QCOMPARE(daemon.tickCount(), quint64(2));
}

void DaemonTest::modeSelectionPersistsAndIsRestored()
{
    {
        Daemon daemon(options());
        QVERIFY(daemon.start());
        QSignalSpy spy(&daemon, &Daemon::modeChanged);
        QCOMPARE(daemon.handleCommand(QStringLiteral("mode quiet")), QStringLiteral("ok quiet"));
        QCOMPARE(spy.count(), 1);
        daemon.tick();
        QCOMPARE(m_tree->read(FakeSysfs::platformProfile()), QStringLiteral("quiet"));
        QCOMPARE(m_tree->read(FakeSysfs::card() + QStringLiteral("/gt_max_freq_mhz")),
                 QStringLiteral("1000"));
    }

    // The selected mode lives in a state file, separate from the mode
    // definitions, so a restart comes back where it left off.
    Daemon restarted(options());
    QVERIFY(restarted.start());
    QCOMPARE(restarted.activeModeId(), QStringLiteral("quiet"));
}

void DaemonTest::unknownModeIsRefused()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    QVERIFY(daemon.handleCommand(QStringLiteral("mode nonexistent"))
                    .startsWith(QStringLiteral("err")));
    QCOMPARE(daemon.activeModeId(), QStringLiteral("work"));
    QVERIFY(daemon.handleCommand(QStringLiteral("mode")).startsWith(QStringLiteral("err")));
}

void DaemonTest::liveTuningAppliesImmediatelyAndIsNotPersisted()
{
    {
        Daemon daemon(options());
        QVERIFY(daemon.start());
        QCOMPARE(daemon.handleCommand(QStringLiteral("set pl1=15 pl2=25 max_perf_pct=30")),
                 QStringLiteral("ok"));
        QVERIFY(daemon.liveTuning());
        daemon.tick();
        QCOMPARE(m_tree->read(FakeSysfs::raplPackage()
                             + QStringLiteral("/constraint_0_power_limit_uw")),
                 QStringLiteral("15000000"));
        QVERIFY(daemon.lastSample().flags & FlagLiveTuning);
        QCOMPARE(daemon.lastSample().pl1W, uint8_t(15));
        // The mode itself is untouched: live values are never written back.
        QCOMPARE(daemon.activeModeId(), QStringLiteral("work"));
    }

    Daemon restarted(options());
    QVERIFY(restarted.start());
    QVERIFY(!restarted.liveTuning());
    QCOMPARE(restarted.requestedSetpoints().pl1W, 20);
}

void DaemonTest::selectingAModeCancelsLiveTuning()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    daemon.handleCommand(QStringLiteral("set pl1=15"));
    QVERIFY(daemon.liveTuning());

    // Last command wins: choosing a mode is a deliberate act.
    daemon.handleCommand(QStringLiteral("mode full"));
    QVERIFY(!daemon.liveTuning());
    QCOMPARE(daemon.requestedSetpoints().pl1W, kMaxConfirmedPl1W);

    daemon.handleCommand(QStringLiteral("set pl1=15"));
    QCOMPARE(daemon.handleCommand(QStringLiteral("reset")), QStringLiteral("ok full"));
    QVERIFY(!daemon.liveTuning());
}

void DaemonTest::liveTuningRefusesNonsense()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    for (const QString &command :
         { QStringLiteral("set"), QStringLiteral("set pl1"), QStringLiteral("set pl1=abc"),
           QStringLiteral("set pl2=abc"), QStringLiteral("set max_perf_pct=abc"),
           QStringLiteral("set gt_max_mhz=abc"), QStringLiteral("set epp=nonsense"),
           QStringLiteral("set profile=nonsense"), QStringLiteral("set nothing=1"),
           QStringLiteral("set =5") }) {
        QVERIFY2(daemon.handleCommand(command).startsWith(QStringLiteral("err")),
                 qPrintable(command));
    }
    QVERIFY(!daemon.liveTuning());

    QCOMPARE(daemon.handleCommand(QStringLiteral("set turbo=on hwp_boost=1 epp=performance "
                                                 "profile=performance gt_max_mhz=1200")),
             QStringLiteral("ok"));
    const Setpoints points = daemon.requestedSetpoints();
    QVERIFY(!points.noTurbo);
    QVERIFY(points.hwpBoost);
    QCOMPARE(points.epp, Epp::Performance);
    QCOMPARE(points.profile, Profile::Performance);
    QCOMPARE(points.gtMaxMhz, 1200);
}

void DaemonTest::liveTuningCannotExceedConfirmedLimits()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    QCOMPARE(daemon.handleCommand(QStringLiteral("set pl1=200 pl2=250 max_perf_pct=400")),
             QStringLiteral("ok"));
    const Setpoints points = daemon.requestedSetpoints();
    QCOMPARE(points.pl1W, kMaxConfirmedPl1W);
    QCOMPARE(points.pl2W, kMaxConfirmedPl2W);
    QCOMPARE(points.maxPerfPct, 100);
}

void DaemonTest::guardOverridesTheModeAndIsRecorded()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    daemon.handleCommand(QStringLiteral("mode full"));

    m_tree->write(FakeSysfs::coretemp() + QStringLiteral("/temp1_input"),
                 QStringLiteral("%1").arg(kGuardFanMaxTempDeci * 100));
    daemon.tick();

    QVERIFY(daemon.lastSample().flags & FlagGuardEngaged);
    QCOMPARE(daemon.lastSample().pl1W, uint8_t(kGuardPl1W));
    // The one place automatic operation drives the fan.
    QCOMPARE(m_tree->read(FakeSysfs::asus() + QStringLiteral("/pwm1_enable")), QStringLiteral("0"));
    // The mode is unchanged: the guard is an interlock, not a mode switch.
    QCOMPARE(daemon.activeModeId(), QStringLiteral("full"));

    m_tree->write(FakeSysfs::coretemp() + QStringLiteral("/temp1_input"), QStringLiteral("600"));
    for (int i = 0; i < kGuardReleaseTicks; ++i)
        daemon.tick();
    QVERIFY(!(daemon.lastSample().flags & FlagGuardEngaged));
    QCOMPARE(m_tree->read(FakeSysfs::asus() + QStringLiteral("/pwm1_enable")), QStringLiteral("2"));
}

void DaemonTest::missingSensorsFallBackToTheMostConservativeMode()
{
    {
        Daemon daemon(options());
        QVERIFY(daemon.start());
        daemon.handleCommand(QStringLiteral("mode full"));
        daemon.tick();
        QCOMPARE(daemon.lastSample().pl1W, uint8_t(kMaxConfirmedPl1W));
    }

    m_tree->remove(FakeSysfs::coretemp() + QStringLiteral("/temp1_input"));
    m_tree->remove(FakeSysfs::coretemp() + QStringLiteral("/temp1_label"));
    Daemon blind(options());
    QVERIFY(blind.start());
    blind.handleCommand(QStringLiteral("mode full"));
    blind.tick();

    // Not the last known mode: the fallback is the most conservative one.
    QCOMPARE(blind.lastSample().pl1W, uint8_t(12));
    QVERIFY(blind.lastSample().flags & FlagSensorsDegraded);
    QCOMPARE(blind.activeModeId(), QStringLiteral("full"));

    // Live tuning does not outrank the fallback: the interlock the experiment
    // was relying on has nothing to act on either.
    blind.handleCommand(QStringLiteral("set pl1=45 max_perf_pct=100"));
    blind.tick();
    QCOMPARE(blind.lastSample().pl1W, uint8_t(12));
    QVERIFY(blind.liveTuning());
    QCOMPARE(blind.requestedSetpoints().pl1W, kMaxConfirmedPl1W);

    // An explicit fan command still gets through: with no temperature reading
    // more airflow is never the wrong answer.
    blind.handleCommand(QStringLiteral("fan max"));
    blind.tick();
    QCOMPARE(blind.lastSample().fanMode, uint8_t(0));
    QCOMPARE(blind.lastSample().pl1W, uint8_t(12));
}

void DaemonTest::marksAreStampedOntoSamples()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    daemon.tick();

    // A judgement made by ear is about what just happened, so it lands on the
    // sample already written as well as on the next one.
    QCOMPARE(daemon.handleCommand(QStringLiteral("mark too-loud")), QStringLiteral("ok"));
    daemon.tick();
    QCOMPARE(daemon.lastSample().mark, uint8_t(Mark::TooLoud));
    daemon.tick();
    QCOMPARE(daemon.lastSample().mark, uint8_t(Mark::None));

    QVERIFY(daemon.handleCommand(QStringLiteral("mark")).startsWith(QStringLiteral("err")));
    QVERIFY(daemon.handleCommand(QStringLiteral("mark splendid"))
                    .startsWith(QStringLiteral("err")));
}

void DaemonTest::fanMaxIsExplicitOnly()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    daemon.tick();
    QCOMPARE(m_tree->read(FakeSysfs::asus() + QStringLiteral("/pwm1_enable")), QStringLiteral("2"));

    QCOMPARE(daemon.handleCommand(QStringLiteral("fan max")), QStringLiteral("ok"));
    daemon.tick();
    QCOMPARE(m_tree->read(FakeSysfs::asus() + QStringLiteral("/pwm1_enable")), QStringLiteral("0"));

    QCOMPARE(daemon.handleCommand(QStringLiteral("fan auto")), QStringLiteral("ok"));
    daemon.tick();
    QCOMPARE(m_tree->read(FakeSysfs::asus() + QStringLiteral("/pwm1_enable")), QStringLiteral("2"));

    QVERIFY(daemon.handleCommand(QStringLiteral("fan")).startsWith(QStringLiteral("err")));
    QVERIFY(daemon.handleCommand(QStringLiteral("fan sideways")).startsWith(QStringLiteral("err")));
}

void DaemonTest::chargeThresholdIsSettable()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    QCOMPARE(daemon.handleCommand(QStringLiteral("charge 80")), QStringLiteral("ok"));
    QCOMPARE(m_tree->read(QStringLiteral(
                     "sys/class/power_supply/BAT0/charge_control_end_threshold")),
             QStringLiteral("80"));
    daemon.tick();
    QCOMPARE(daemon.lastSample().chargeLimit, uint8_t(80));

    QVERIFY(daemon.handleCommand(QStringLiteral("charge")).startsWith(QStringLiteral("err")));
    QVERIFY(daemon.handleCommand(QStringLiteral("charge lots")).startsWith(QStringLiteral("err")));
}

void DaemonTest::configurationIsRereadOnCommand()
{
    writeConfig(QStringLiteral("[general]\ndefault_mode=custom\n"
                               "[mode custom]\nlabel=Custom\npl1_w=17\nplatform_profile=quiet\n"));
    Daemon daemon(options());
    QVERIFY(daemon.start());
    QCOMPARE(daemon.activeModeId(), QStringLiteral("custom"));
    QCOMPARE(daemon.requestedSetpoints().pl1W, 17);

    writeConfig(QStringLiteral("[general]\ndefault_mode=custom\n"
                               "[mode custom]\nlabel=Custom\npl1_w=23\nplatform_profile=quiet\n"));
    QCOMPARE(daemon.handleCommand(QStringLiteral("reload")), QStringLiteral("ok"));
    QCOMPARE(daemon.requestedSetpoints().pl1W, 23);

    // A configuration that stops defining the active mode falls back rather
    // than leaving the machine unmanaged.
    writeConfig(QStringLiteral("[mode other]\npl1_w=11\n"));
    daemon.handleCommand(QStringLiteral("reload"));
    QCOMPARE(daemon.activeModeId(), QStringLiteral("other"));

    QFile::remove(m_work.path() + QStringLiteral("/settings.conf"));
    daemon.handleCommand(QStringLiteral("reload"));
    QCOMPARE(daemon.modes().modes().size(), 3);
}

void DaemonTest::unknownCommandsAreRejected()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    QVERIFY(daemon.handleCommand(QStringLiteral("levitate")).startsWith(QStringLiteral("err")));
    QVERIFY(daemon.handleCommand(QStringLiteral("   ")).startsWith(QStringLiteral("err")));

    const QJsonObject status = statusOf(daemon);
    QCOMPARE(status.value(QStringLiteral("mode")).toString(), QStringLiteral("work"));
    QCOMPARE(status.value(QStringLiteral("modes")).toArray().size(), 3);
    QCOMPARE(status.value(QStringLiteral("live_tuning")).toBool(), false);
    QCOMPARE(status.value(QStringLiteral("requested")).toObject()
                     .value(QStringLiteral("pl1_w")).toInt(),
             20);
}

void DaemonTest::shutdownRestoresTheHardware()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    daemon.handleCommand(QStringLiteral("fan max"));
    daemon.tick();
    QCOMPARE(m_tree->read(FakeSysfs::asus() + QStringLiteral("/pwm1_enable")), QStringLiteral("0"));

    QSignalSpy spy(&daemon, &Daemon::finished);
    daemon.shutdown();
    QCOMPARE(spy.count(), 1);

    // A fan left behind by a stopped daemon is a defect; nothing else picks up
    // after this process.
    QCOMPARE(m_tree->read(FakeSysfs::asus() + QStringLiteral("/pwm1_enable")), QStringLiteral("2"));
    QCOMPARE(m_tree->read(FakeSysfs::raplPackage()
                         + QStringLiteral("/constraint_0_power_limit_uw")),
             QString::number(qint64(kMaxConfirmedPl1W) * 1000000));

    // Shutting down twice is not an error, which is what makes the destructor
    // and the signal path able to share one exit.
    daemon.shutdown();
    QCOMPARE(spy.count(), 1);
}

void DaemonTest::runsOverTheControlSocket()
{
    DaemonOptions o = options();
    Daemon daemon(o);
    QVERIFY(daemon.start());

    QLocalSocket client;
    client.connectToServer(o.socketPath);
    QVERIFY(client.waitForConnected(2000));
    client.write("mode quiet\nstatus\n");
    client.flush();

    QStringList replies;
    QDeadlineTimer deadline(3000);
    while (replies.size() < 2 && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        while (client.canReadLine())
            replies.append(QString::fromUtf8(client.readLine()).trimmed());
    }
    QCOMPARE(replies.size(), 2);
    QCOMPARE(replies.at(0), QStringLiteral("ok quiet"));
    QVERIFY(replies.at(1).startsWith(QStringLiteral("ok {")));
    QCOMPARE(QJsonDocument::fromJson(replies.at(1).mid(3).toUtf8())
                     .object()
                     .value(QStringLiteral("mode"))
                     .toString(),
             QStringLiteral("quiet"));
}

void DaemonTest::stopsAfterTheRequestedNumberOfTicks()
{
    DaemonOptions o = options();
    o.maxTicks = 3;
    o.intervalMs = 5;
    Daemon daemon(o);
    QVERIFY(daemon.start());

    QSignalSpy spy(&daemon, &Daemon::finished);
    QDeadlineTimer deadline(3000);
    while (spy.isEmpty() && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(daemon.tickCount(), quint64(3));

    // The samples reached the ring and are readable through the export path,
    // which is the only interface to them.
    MetricsRing ring;
    QVERIFY(ring.openExisting(o.ringPath));
    QCOMPARE(ring.count(), uint64_t(3));
    QString text;
    QTextStream stream(&text);
    QCOMPARE(exporter::write(ring, stream, exporter::Format::Csv, 0, 0), quint64(3));
}

void DaemonTest::theChargeThresholdIsReappliedAtStart()
{
    const QString node =
            QStringLiteral("sys/class/power_supply/BAT0/charge_control_end_threshold");
    {
        Daemon daemon(options());
        QVERIFY(daemon.start());
        QCOMPARE(daemon.handleCommand(QStringLiteral("charge 80")), QStringLiteral("ok"));
        QCOMPARE(daemon.state().chargeLimit, 80);
    }

    // The threshold lives in sysfs and a reboot loses it. Holding it is an
    // obligation inherited with sole ownership of the power controls, and
    // nothing else will put it back.
    m_tree->write(node, QStringLiteral("100"));
    {
        Daemon restarted(options());
        QVERIFY(restarted.start());
        QCOMPARE(m_tree->read(node), QStringLiteral("80"));
    }

    // A machine without charge control says so and carries on.
    m_tree->remove(node);
    Daemon without(options());
    QVERIFY(without.start());
}

void DaemonTest::theBacklightIsLeftAloneUntilTheAutomationIsSwitchedOn()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());

    const int before = backlightRaw();
    for (int i = 0; i < 20; ++i) {
        setLight(i % 2 == 0 ? 50 : 6000);
        daemon.tick();
        finishFade(daemon);
    }

    // Off by default, and off means the daemon does not write the backlight at
    // all — not that it moves it less.
    QCOMPARE(backlightRaw(), before);
    QVERIFY(!(daemon.lastSample().flags & FlagAutoBrightness));
    // The readings are recorded either way, because a flat trace with no light
    // reading beside it cannot be judged.
    QCOMPARE(daemon.lastSample().brightness, uint8_t(50));
    QCOMPARE(daemon.lastSample().light, uint16_t(6000));
}

void DaemonTest::switchingTheAutomationOnOrOffDoesNotMoveTheBacklight()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());

    const int before = backlightRaw();
    QCOMPARE(daemon.handleCommand(QStringLiteral("brightness on")), QStringLiteral("ok"));
    daemon.tick();
    finishFade(daemon);
    // Switching on adopts a position; it does not jump to it. A jump in answer
    // to a toggle is the same defect as one in answer to a profile switch.
    QCOMPARE(backlightRaw(), before);
    QVERIFY(daemon.lastSample().flags & FlagAutoBrightness);

    QCOMPARE(daemon.handleCommand(QStringLiteral("brightness off")), QStringLiteral("ok"));
    daemon.tick();
    finishFade(daemon);
    QCOMPARE(backlightRaw(), before);
}

void DaemonTest::theBacklightFollowsTheLightOnceItHasChanged()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    daemon.handleCommand(QStringLiteral("brightness on"));

    // One tick to adopt a position at the current light.
    setLight(1000);
    daemon.tick();
    const int seeded = backlightRaw();

    // Unchanging light produces no writes at all.
    for (int i = 0; i < 30; ++i)
        daemon.tick();
    QCOMPARE(backlightRaw(), seeded);
    QVERIFY(!daemon.fading());

    // A burst shorter than the dwell is ignored entirely.
    setLight(103000);
    for (int i = 0; i < kBrightnessDwellTicks - 1; ++i)
        daemon.tick();
    QVERIFY(!daemon.fading());
    setLight(25000);
    daemon.tick();
    QCOMPARE(backlightRaw(), seeded);

    // A change that holds moves once the dwell has passed.
    setLight(103000);
    QCOMPARE(tickUntilMove(daemon, kBrightnessDwellTicks + 2), kBrightnessDwellTicks);
    finishFade(daemon);
    QVERIFY(backlightRaw() > seeded);

    // And lands inside the window around the level the person set.
    daemon.tick();
    const Sample &s = daemon.lastSample();
    // The base is the level with the light's contribution taken out, so at a
    // light the curve does not call neutral it differs from what was set.
    QVERIFY(s.brightnessBase != kAbsentPercent);
    QVERIFY(s.brightness >= s.brightnessBase - kBrightnessDefaultResponsePct
            && s.brightness <= std::min(100, s.brightnessBase
                                                     + kBrightnessDefaultResponsePct));
}

void DaemonTest::theFadeMovesInSeveralStepsRatherThanOne()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    daemon.handleCommand(QStringLiteral("brightness on"));
    setLight(50);
    daemon.tick();

    setLight(6000);
    QVERIFY(tickUntilMove(daemon, kBrightnessDwellTicks + 2) > 0);

    // A correct step applied in one write still reads as a defect, so the move
    // is spread over several.
    int writes = 0;
    int previous = backlightRaw();
    for (int i = 0; i < 1000 && daemon.fading(); ++i) {
        daemon.fadeBrightness();
        if (backlightRaw() != previous) {
            previous = backlightRaw();
            ++writes;
        }
    }
    QVERIFY2(writes > 1, qPrintable(QStringLiteral("writes=%1").arg(writes)));
    QVERIFY(!daemon.fading());
}

void DaemonTest::aHandCorrectionBecomesTheNewBase()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    daemon.handleCommand(QStringLiteral("brightness on"));
    daemon.tick();

    // Moving the brightness is a statement about what is wanted in this light,
    // so the base is re-derived from it: what was asked for, with the light's
    // contribution of that moment taken back out.
    setBacklightByHand(FakeSysfs::kBacklightMax * 3 / 4);
    daemon.tick();
    const int base = daemon.state().brightnessBase;
    QVERIFY(base != DaemonState::kBrightnessBaseUnset);

    const QJsonObject brightness =
            statusOf(daemon).value(QStringLiteral("brightness")).toObject();
    QCOMPARE(brightness.value(QStringLiteral("base")).toInt(), base);
    // The band moved with the correction, and the ends of the scale cut it
    // rather than shifting it.
    QCOMPARE(brightness.value(QStringLiteral("window_low")).toInt(),
             std::max(kBrightnessDefaultFloorPct, base - kBrightnessDefaultResponsePct));
    QCOMPARE(brightness.value(QStringLiteral("window_high")).toInt(),
             std::min(100, base + kBrightnessDefaultResponsePct));

    // The correction is not immediately argued with: the automation adopts the
    // new position and waits for the light to change.
    const int corrected = backlightRaw();
    for (int i = 0; i < 20; ++i) {
        daemon.tick();
        finishFade(daemon);
    }
    QCOMPARE(backlightRaw(), corrected);

    // And it survives a restart, so the level does not have to be set again
    // after every boot.
    QFile file(m_work.path() + QStringLiteral("/state"));
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QVERIFY(QString::fromUtf8(file.readAll())
                    .contains(QStringLiteral("brightness_base=%1").arg(base)));
}

void DaemonTest::theFloorBindsEverybodyIncludingThePerson()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    QCOMPARE(daemon.handleCommand(QStringLiteral("config set brightness floor_pct=30")),
             QStringLiteral("ok"));
    daemon.handleCommand(QStringLiteral("brightness on"));
    setLight(25000);
    daemon.tick();

    // A level dragged below the floor is pulled back to it. The floor is the
    // one bound that binds everybody: a screen driven into unreadable is a
    // failure whoever did the driving.
    setBacklightByHand(FakeSysfs::kBacklightMax / 20);
    daemon.tick();
    QVERIFY(daemon.lastSample().brightness >= 30);
    // And the base is derived from what was applied rather than from what was
    // asked for, so there is no hidden number behind the screen.
    QVERIFY(daemon.state().brightnessBase >= 30);

    // The ceiling is 100 out of the box, so a hand at the top of the scale stays
    // at the top and the automation may take the screen there in a bright room.
    setBacklightByHand(FakeSysfs::kBacklightMax);
    daemon.tick();
    for (int i = 0; i < 20; ++i) {
        daemon.tick();
        finishFade(daemon);
    }
    QCOMPARE(backlightRaw(), FakeSysfs::kBacklightMax);
}

void DaemonTest::theCeilingBindsEverybodyTheSameWay()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    QCOMPARE(daemon.handleCommand(QStringLiteral("config set brightness ceiling_pct=60")),
             QStringLiteral("ok"));
    daemon.handleCommand(QStringLiteral("brightness on"));
    setLight(25000);
    daemon.tick();

    // Asked for the whole scale, given the ceiling — and the reply says which.
    QCOMPARE(daemon.handleCommand(QStringLiteral("brightness level 100")),
             QStringLiteral("ok 60"));
    QVERIFY(daemon.state().brightnessBase <= 60);

    // A hand at the top of the scale is pulled back down to it, the same way the
    // floor pulls a hand up.
    setBacklightByHand(FakeSysfs::kBacklightMax);
    daemon.tick();
    QVERIFY(daemon.lastSample().brightness <= 60);

    // In force means in the record.
    QCOMPARE(daemon.lastSample().brightnessCeiling, uint8_t(60));

    // The brightest room the curve knows does not take the screen past it.
    setLight(400000);
    for (int i = 0; i < 30; ++i) {
        daemon.tick();
        finishFade(daemon);
    }
    QVERIFY(backlightRaw() <= FakeSysfs::kBacklightMax * 60 / 100);

    // And it is in both answers, beside the floor.
    const QJsonObject status =
            QJsonDocument::fromJson(daemon.handleCommand(QStringLiteral("status")).mid(3).toUtf8())
                    .object()
                    .value(QStringLiteral("brightness"))
                    .toObject();
    QCOMPARE(status.value(QStringLiteral("ceiling_pct")).toInt(), 60);
    const QJsonObject configured =
            QJsonDocument::fromJson(
                    daemon.handleCommand(QStringLiteral("config show")).mid(3).toUtf8())
                    .object()
                    .value(QStringLiteral("brightness"))
                    .toObject();
    QCOMPARE(configured.value(QStringLiteral("ceiling_pct")).toInt(), 60);
}

void DaemonTest::selectingAModeDoesNotMoveTheBacklight()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    daemon.handleCommand(QStringLiteral("brightness on"));
    daemon.tick();
    const int before = backlightRaw();

    // Bundling the backlight with the power profile is vendor behaviour this
    // project treats as a defect, so this is tested rather than assumed.
    for (const QString &mode : { QStringLiteral("quiet"), QStringLiteral("full"),
                                 QStringLiteral("work") }) {
        daemon.handleCommand(QStringLiteral("mode ") + mode);
        daemon.tick();
        finishFade(daemon);
        QCOMPARE(backlightRaw(), before);
    }
}

void DaemonTest::brightnessChoicesSurviveARestart()
{
    int savedBase = 0;
    {
        Daemon daemon(options());
        QVERIFY(daemon.start());
        QCOMPARE(daemon.handleCommand(QStringLiteral("brightness on")), QStringLiteral("ok"));
        setBacklightByHand(FakeSysfs::kBacklightMax / 4);
        daemon.tick();
        QVERIFY(daemon.state().brightnessBase != DaemonState::kBrightnessBaseUnset);
        savedBase = daemon.state().brightnessBase;
    }

    Daemon restarted(options());
    QVERIFY(restarted.start());
    QCOMPARE(restarted.state().autoBrightness, true);
    QCOMPARE(restarted.state().brightnessBase, savedBase);

    // Something switched off must not come back on by itself either.
    restarted.handleCommand(QStringLiteral("brightness off"));
    restarted.shutdown();
    Daemon again(options());
    QVERIFY(again.start());
    QCOMPARE(again.state().autoBrightness, false);
}

void DaemonTest::brightnessCommandsAreValidated()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    for (const QString &command :
         { QStringLiteral("brightness"), QStringLiteral("brightness sideways"),
           QStringLiteral("brightness level"), QStringLiteral("brightness level plenty"),
           QStringLiteral("brightness level 101"), QStringLiteral("brightness level -1"),
           QStringLiteral("brightness set response_pct"),
           QStringLiteral("brightness set response_pct=wide"),
           QStringLiteral("brightness set sideways_pct=10") }) {
        QVERIFY2(daemon.handleCommand(command).startsWith(QStringLiteral("err")),
                 qPrintable(command));
    }
    QCOMPARE(daemon.state().autoBrightness, false);

    // A refused command changes nothing, so the profile's own numbers are
    // still the ones in force.
    const QJsonObject brightness =
            statusOf(daemon).value(QStringLiteral("brightness")).toObject();
    QCOMPARE(brightness.value(QStringLiteral("response_pct")).toInt(),
             kBrightnessDefaultResponsePct);
}

void DaemonTest::theBrightnessWindowIsTunedLive()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    daemon.handleCommand(QStringLiteral("brightness on"));
    setBacklightByHand(FakeSysfs::kBacklightMax / 2);
    daemon.tick();
    const int base = daemon.state().brightnessBase;

    // As text, because the pair is only ever compared as a whole here.
    const auto window = [&daemon] {
        const QJsonObject brightness =
                statusOf(daemon).value(QStringLiteral("brightness")).toObject();
        return QStringLiteral("%1..%2")
                .arg(brightness.value(QStringLiteral("window_low")).toInt())
                .arg(brightness.value(QStringLiteral("window_high")).toInt());
    };
    QCOMPARE(window(),
             QStringLiteral("%1..%2")
                     .arg(std::max(kBrightnessDefaultFloorPct,
                                   base - kBrightnessDefaultResponsePct))
                     .arg(std::min(100, base + kBrightnessDefaultResponsePct)));

    // Widening the reaction is a judgement made by eye, and it applies at once.
    // The base moves with it: a wider reaction means the light of this moment
    // was contributing more, so the level on screen implies a different base.
    QCOMPARE(daemon.handleCommand(QStringLiteral("brightness set response_pct=40")),
             QStringLiteral("ok"));
    const int widened = daemon.state().brightnessBase;
    QCOMPARE(window(),
             QStringLiteral("%1..%2")
                     .arg(std::max(kBrightnessDefaultFloorPct, widened - 40))
                     .arg(std::min(100, widened + 40)));

    // The floor binds everybody, so it cuts the band from below.
    QCOMPARE(daemon.handleCommand(QStringLiteral("brightness set floor_pct=35")),
             QStringLiteral("ok"));
    QVERIFY(window().startsWith(QStringLiteral("35..")));

    // Every value in force is in the record, because a sample that needs
    // another file read to be understood is not self-contained.
    daemon.tick();
    const Sample &s = daemon.lastSample();
    QCOMPARE(s.brightnessResponse, uint8_t(40));
    QCOMPARE(s.brightnessFloor, uint8_t(35));

    // A floor above the level pulls the screen up to it: it binds the person
    // as well, which is the whole point of having one.
    setBacklightByHand(FakeSysfs::kBacklightMax / 10);
    daemon.tick();
    QVERIFY(daemon.lastSample().brightness >= 35);
}

void DaemonTest::telemetryDepthIsASettingAndSwitchingItOffKeepsTheFile()
{
    // Without --capacity the depth comes from the settings, which is the
    // production case; the rest of this suite overrides it to sixty-four records.
    DaemonOptions o = options();
    o.ringCapacity = 0;
    Daemon daemon(o);
    QVERIFY(daemon.start());

    const auto metrics = [&daemon] {
        return QJsonDocument::fromJson(
                       daemon.handleCommand(QStringLiteral("status")).mid(3).toUtf8())
                .object()
                .value(QStringLiteral("metrics"))
                .toObject();
    };
    QCOMPARE(metrics().value(QStringLiteral("retention_days")).toInt(), kRetentionDefaultDays);
    QVERIFY(metrics().value(QStringLiteral("enabled")).toBool());

    daemon.tick();
    daemon.tick();
    QCOMPARE(metrics().value(QStringLiteral("samples")).toInt(), 2);

    // A shallower ring is a smaller file, and the samples come across rather than
    // being thrown away: a change of depth is the same operation as a change of
    // layout.
    QCOMPARE(daemon.handleCommand(QStringLiteral("config set metrics retention_days=1")),
             QStringLiteral("ok"));
    QCOMPARE(metrics().value(QStringLiteral("retention_days")).toInt(), 1);
    QCOMPARE(metrics().value(QStringLiteral("samples")).toInt(), 2);
    QCOMPARE(QFileInfo(o.ringPath).size(),
             qint64(sizeof(RingHeader) + kSamplesPerDay * sizeof(Sample)));

    // Out of range is clamped and reported rather than refused.
    const QString reply = daemon.handleCommand(QStringLiteral("config set metrics "
                                                              "retention_days=400"));
    QVERIFY2(reply.startsWith(QStringLiteral("ok ")), qPrintable(reply));
    QCOMPARE(metrics().value(QStringLiteral("retention_days")).toInt(), kRetentionMaxDays);

    // Switched off, nothing is appended — and nothing is deleted either. The
    // sample is still taken, so the readings keep answering.
    QCOMPARE(daemon.handleCommand(QStringLiteral("config set metrics enabled=off")),
             QStringLiteral("ok"));
    const int before = metrics().value(QStringLiteral("samples")).toInt();
    daemon.tick();
    daemon.tick();
    QCOMPARE(metrics().value(QStringLiteral("samples")).toInt(), before);
    QVERIFY(QFileInfo::exists(o.ringPath));
    QVERIFY(daemon.lastSample().ts != 0);

    daemon.handleCommand(QStringLiteral("config set metrics enabled=on"));
    daemon.tick();
    QCOMPARE(metrics().value(QStringLiteral("samples")).toInt(), before + 1);

    // Both keys are in the saved file, and a reset takes them back.
    MetricsSettings saved = builtinMetricsSettings();
    QVERIFY(loadMetricsSettings(o.configPath, &saved));
    QCOMPARE(saved.retentionDays, kRetentionMaxDays);
    QCOMPARE(daemon.handleCommand(QStringLiteral("config reset metrics")), QStringLiteral("ok"));
    QCOMPARE(metrics().value(QStringLiteral("retention_days")).toInt(), kRetentionDefaultDays);

    for (const QString &line : { QStringLiteral("config set metrics"),
                                 QStringLiteral("config set metrics retention_days"),
                                 QStringLiteral("config set metrics enabled=maybe"),
                                 QStringLiteral("config set metrics depth=7"),
                                 QStringLiteral("config reset sideways") }) {
        QVERIFY2(daemon.handleCommand(line).startsWith(QStringLiteral("err")), qPrintable(line));
    }
}

void DaemonTest::theBacklightSettingsAreDurable()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());

    const auto response = [&daemon] {
        return statusOf(daemon)
                .value(QStringLiteral("brightness"))
                .toObject()
                .value(QStringLiteral("response_pct"))
                .toInt();
    };

    // `brightness set` applies and nothing more: dragging a slider must not
    // write a file thirty times on the way across.
    QCOMPARE(daemon.handleCommand(QStringLiteral("brightness set response_pct=40")),
             QStringLiteral("ok"));
    QCOMPARE(response(), 40);
    QVERIFY(!QFileInfo::exists(options().configPath));

    // `config set brightness` is the one that keeps it, and it survives a
    // restart — which is what makes these settings rather than an experiment.
    QCOMPARE(daemon.handleCommand(QStringLiteral("config set brightness response_pct=45")),
             QStringLiteral("ok"));
    QVERIFY(QFileInfo::exists(options().configPath));
    daemon.shutdown();

    Daemon restarted(options());
    QVERIFY(restarted.start());
    QCOMPARE(statusOf(restarted)
                     .value(QStringLiteral("brightness"))
                     .toObject()
                     .value(QStringLiteral("response_pct"))
                     .toInt(),
             45);
}

void DaemonTest::theBacklightPathDegradesOnItsOwn()
{
    m_tree->remove(FakeSysfs::backlight() + QStringLiteral("/brightness"));
    m_tree->remove(FakeSysfs::als() + QStringLiteral("/in_illuminance_raw"));

    Daemon daemon(options());
    QString error;
    // No backlight and no sensor is not a reason to stop managing heat.
    QVERIFY2(daemon.start(&error), qPrintable(error));
    QCOMPARE(daemon.handleCommand(QStringLiteral("brightness on")), QStringLiteral("ok"));
    daemon.tick();

    const Sample &s = daemon.lastSample();
    QCOMPARE(s.brightness, kAbsentPercent);
    QCOMPARE(s.brightnessBase, kAbsentPercent);
    QCOMPARE(s.light, kAbsentLight);
    QCOMPARE(m_tree->read(FakeSysfs::raplPackage()
                          + QStringLiteral("/constraint_0_power_limit_uw")),
             QStringLiteral("20000000"));

    const QJsonObject brightness =
            statusOf(daemon).value(QStringLiteral("brightness")).toObject();
    QCOMPARE(brightness.value(QStringLiteral("available")).toBool(), false);
}

void DaemonTest::configShowAnswersWithWhatIsSaved()
{
    Daemon daemon(options());
    QString error;
    QVERIFY2(daemon.start(&error), qPrintable(error));

    const QString reply = daemon.handleCommand(QStringLiteral("config show"));
    QVERIFY(reply.startsWith(QLatin1String("ok ")));
    const QJsonObject config = QJsonDocument::fromJson(reply.mid(3).toUtf8()).object();
    // Nothing has been changed yet, so the built-in values are the answer and
    // there is no file behind them.
    QCOMPARE(config.value(QStringLiteral("stored")).toBool(), false);
    QCOMPARE(config.value(QStringLiteral("default_mode")).toString(), QStringLiteral("work"));
    QCOMPARE(config.value(QStringLiteral("modes")).toArray().size(), 3);
    const QJsonObject brightness = config.value(QStringLiteral("brightness")).toObject();
    QCOMPARE(brightness.value(QStringLiteral("response_pct")).toInt(),
             kBrightnessDefaultResponsePct);
    // Lux on the wire, and a curve whose last point is daylight rather than a
    // lit room, because the sensor reads well past a lit room.
    const QString curve = brightness.value(QStringLiteral("curve")).toString();
    QVERIFY2(curve.contains(QLatin1String("400:100")), qPrintable(curve));
}

void DaemonTest::theBaselineIsSetOutrightAndSurvivesARestart()
{
    Daemon daemon(options());
    QString error;
    QVERIFY2(daemon.start(&error), qPrintable(error));
    QVERIFY(daemon.handleCommand(QStringLiteral("brightness on"))
                    .startsWith(QLatin1String("ok")));

    // Set outright, and the level follows from it and the light of the moment —
    // the reverse of `brightness level`, where the level is given and the
    // baseline is derived from it.
    QCOMPARE(daemon.handleCommand(QStringLiteral("brightness base 64")), QStringLiteral("ok 64"));
    QJsonObject brightness = statusOf(daemon).value(QLatin1String("brightness")).toObject();
    QCOMPARE(brightness.value(QLatin1String("base")).toInt(), 64);
    const int level = brightness.value(QLatin1String("level")).toInt();
    QVERIFY2(level < 64, "the fake tree is a dark room, so the room subtracts");

    // The floor binds the baseline as it binds everything else about the
    // backlight: a screen driven into unreadable is a failure, not a saving.
    QVERIFY(daemon.handleCommand(QStringLiteral("config set brightness floor_pct=40"))
                    .startsWith(QLatin1String("ok")));
    QCOMPARE(daemon.handleCommand(QStringLiteral("brightness base 5")), QStringLiteral("ok 40"));

    // Refusals, each naming what was wrong with it.
    QVERIFY(daemon.handleCommand(QStringLiteral("brightness base"))
                    .contains(QLatin1String("percentage")));
    QVERIFY(daemon.handleCommand(QStringLiteral("brightness base half"))
                    .contains(QLatin1String("not a number")));
    QVERIFY(daemon.handleCommand(QStringLiteral("brightness base 140"))
                    .contains(QLatin1String("outside 0..100")));

    // Kept in the state file, so a reboot does not lose the level the person
    // works around. It is not a setting: it is what they were last looking at.
    QVERIFY(daemon.handleCommand(QStringLiteral("brightness base 70"))
                    .startsWith(QLatin1String("ok")));
    daemon.shutdown();

    Daemon restarted(options());
    QVERIFY2(restarted.start(&error), qPrintable(error));
    QCOMPARE(restarted.state().brightnessBase, 70);
    restarted.shutdown();
}

void DaemonTest::editingAModeIsDurableAndAppliesAtOnce()
{
    Daemon daemon(options());
    QString error;
    QVERIFY2(daemon.start(&error), qPrintable(error));
    QCOMPARE(daemon.handleCommand(QStringLiteral("mode work")), QStringLiteral("ok work"));

    QCOMPARE(daemon.handleCommand(QStringLiteral("config set mode work pl1_w=17 pl2_w=25")),
             QStringLiteral("ok"));
    // Applied without waiting for the next tick: an edit that appears to do
    // nothing for a second reads as an edit that did not take.
    QCOMPARE(m_tree->read(FakeSysfs::raplPackage()
                          + QStringLiteral("/constraint_0_power_limit_uw")),
             QStringLiteral("17000000"));

    // And durable: a second daemon reading the same path sees it.
    daemon.shutdown();
    Daemon again(options());
    QVERIFY2(again.start(&error), qPrintable(error));
    const Mode *mode = again.modes().find(QStringLiteral("work"));
    QVERIFY(mode);
    QCOMPARE(mode->setpoints.pl1W, 17);
    QCOMPARE(mode->setpoints.pl2W, 25);
}

void DaemonTest::modesAreEditedNotCreated()
{
    Daemon daemon(options());
    QString error;
    QVERIFY2(daemon.start(&error), qPrintable(error));

    const QString reply = daemon.handleCommand(QStringLiteral("config set mode invented pl1_w=10"));
    QVERIFY(reply.startsWith(QLatin1String("err unknown mode invented")));
    // The answer names what does exist, because a refusal that leaves you
    // guessing costs a second command.
    QVERIFY(reply.contains(QStringLiteral("quiet")));
    QCOMPARE(daemon.modes().modes().size(), 3);
}

void DaemonTest::aRefusedEditChangesNothingOnDiskOrInMemory()
{
    Daemon daemon(options());
    QString error;
    QVERIFY2(daemon.start(&error), qPrintable(error));
    const int before = daemon.modes().find(QStringLiteral("work"))->setpoints.pl1W;

    QVERIFY(daemon.handleCommand(QStringLiteral("config set mode work pl1_w=nonsense"))
                    .startsWith(QLatin1String("err")));
    QVERIFY(daemon.handleCommand(QStringLiteral("config set mode work invented=1"))
                    .startsWith(QLatin1String("err")));
    QCOMPARE(daemon.modes().find(QStringLiteral("work"))->setpoints.pl1W, before);
    // Nothing was written either: a refused command must not leave a file
    // behind that says something happened.
    QVERIFY(!QFileInfo::exists(options().configPath));

    // Out of range is a different matter: clamped, applied, and reported so
    // that nobody tunes a number that never took effect.
    const QString clamped = daemon.handleCommand(QStringLiteral("config set mode work pl1_w=900"));
    QVERIFY(clamped.startsWith(QLatin1String("ok ")));
    QVERIFY(clamped.contains(QStringLiteral("outside")));
    QCOMPARE(daemon.modes().find(QStringLiteral("work"))->setpoints.pl1W, kMaxConfirmedPl1W);
}

void DaemonTest::liveTuningIsSavedIntoTheModeThatIsNamed()
{
    Daemon daemon(options());
    QString error;
    QVERIFY2(daemon.start(&error), qPrintable(error));
    QCOMPARE(daemon.handleCommand(QStringLiteral("mode work")), QStringLiteral("ok work"));

    QVERIFY(daemon.handleCommand(QStringLiteral("config save live to work"))
                    .startsWith(QLatin1String("err")));

    QCOMPARE(daemon.handleCommand(QStringLiteral("set pl1=13 max_perf_pct=44")),
             QStringLiteral("ok"));
    QVERIFY(daemon.liveTuning());

    // Into a mode that is not the one in force: the definition changes, the
    // overlay stays, and the machine does not move.
    QCOMPARE(daemon.handleCommand(QStringLiteral("config save live to quiet")),
             QStringLiteral("ok"));
    QCOMPARE(daemon.modes().find(QStringLiteral("quiet"))->setpoints.pl1W, 13);
    QVERIFY(daemon.liveTuning());

    // Into the mode in force: the applied values do not change, so the overlay
    // is dropped and the status stops claiming an experiment is running.
    QCOMPARE(daemon.handleCommand(QStringLiteral("config save live to work")),
             QStringLiteral("ok"));
    QCOMPARE(daemon.modes().find(QStringLiteral("work"))->setpoints.maxPerfPct, 44);
    QVERIFY(!daemon.liveTuning());
    QCOMPARE(daemon.requestedSetpoints().pl1W, 13);
}

void DaemonTest::resettingReturnsToTheBuiltInValues()
{
    Daemon daemon(options());
    QString error;
    QVERIFY2(daemon.start(&error), qPrintable(error));

    QCOMPARE(daemon.handleCommand(QStringLiteral("config set mode work pl1_w=41 pl2_w=50")),
             QStringLiteral("ok"));
    QCOMPARE(daemon.handleCommand(QStringLiteral("config set brightness response_pct=7")),
             QStringLiteral("ok"));
    QCOMPARE(daemon.handleCommand(QStringLiteral("set pl1=9")), QStringLiteral("ok"));

    QVERIFY(daemon.handleCommand(QStringLiteral("config reset sideways"))
                    .startsWith(QLatin1String("err")));

    QCOMPARE(daemon.handleCommand(QStringLiteral("config reset modes")), QStringLiteral("ok"));
    QCOMPARE(daemon.modes().find(QStringLiteral("work"))->setpoints.pl1W,
             ModeTable::builtinDefaults().find(QStringLiteral("work"))->setpoints.pl1W);
    // A reset is the way out of a mess, so the experiment goes with it.
    QVERIFY(!daemon.liveTuning());
    // Only the group that was named: the brightness edit is still there.
    QString reply = daemon.handleCommand(QStringLiteral("config show"));
    QJsonObject config = QJsonDocument::fromJson(reply.mid(3).toUtf8()).object();
    QCOMPARE(config.value(QStringLiteral("brightness"))
                     .toObject()
                     .value(QStringLiteral("response_pct"))
                     .toInt(),
             7);

    QCOMPARE(daemon.handleCommand(QStringLiteral("config reset all")), QStringLiteral("ok"));
    reply = daemon.handleCommand(QStringLiteral("config show"));
    config = QJsonDocument::fromJson(reply.mid(3).toUtf8()).object();
    QCOMPARE(config.value(QStringLiteral("brightness"))
                     .toObject()
                     .value(QStringLiteral("response_pct"))
                     .toInt(),
             kBrightnessDefaultResponsePct);
}

void DaemonTest::theDisplayCountIsRecorded()
{
    Daemon daemon(options());
    QString error;
    QVERIFY2(daemon.start(&error), qPrintable(error));

    daemon.tick();
    // The fake tree has the internal panel connected and two empty sockets.
    QCOMPARE(daemon.lastSample().displays, 1);

    m_tree->write(FakeSysfs::connector(QStringLiteral("DP-1")), QStringLiteral("connected"));
    daemon.tick();
    QCOMPARE(daemon.lastSample().displays, 2);
    QCOMPARE(statusOf(daemon)
                     .value(QStringLiteral("sensors"))
                     .toObject()
                     .value(QStringLiteral("displays"))
                     .toInt(),
             2);

    // No DRM tree at all is "nobody asked", not "nothing plugged in". The first
    // daemon goes first: two owners of one control surface is the one thing
    // this architecture forbids, and its socket is the evidence.
    daemon.shutdown();
    m_tree->remove(FakeSysfs::connector(QStringLiteral("eDP-1")));
    m_tree->remove(FakeSysfs::connector(QStringLiteral("DP-1")));
    m_tree->remove(FakeSysfs::connector(QStringLiteral("HDMI-A-1")));
    Daemon bare(options());
    QVERIFY2(bare.start(&error), qPrintable(error));
    bare.tick();
    QCOMPARE(bare.lastSample().displays, kAbsentPercent);
}

QTEST_MAIN(DaemonTest)
#include "daemon_test.moc"
