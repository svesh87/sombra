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
    void snippetReproducesTheLiveSetpoints();
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
    void theBoundsConstrainTheAutomationAndNotThePerson();
    void selectingAModeDoesNotMoveTheBacklight();
    void brightnessChoicesSurviveARestart();
    void brightnessCommandsAreValidated();
    void theBrightnessWindowIsTunedLive();
    void theBrightnessOverlayBelongsToItsProfile();
    void theBacklightPathDegradesOnItsOwn();

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
    QFile::remove(m_work.path() + QStringLiteral("/modes.conf"));
}

DaemonOptions DaemonTest::options() const
{
    DaemonOptions o;
    o.sysfsRoot = m_tree->root();
    o.configPath = m_work.path() + QStringLiteral("/modes.conf");
    o.statePath = m_work.path() + QStringLiteral("/state");
    o.ringPath = m_work.path() + QStringLiteral("/metrics.ring");
    o.socketPath = m_work.path() + QStringLiteral("/control.sock");
    o.ringCapacity = 64;
    o.flushSeconds = 2;
    return o;
}

QString DaemonTest::writeConfig(const QString &contents) const
{
    const QString path = m_work.path() + QStringLiteral("/modes.conf");
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

    QFile::remove(m_work.path() + QStringLiteral("/modes.conf"));
    daemon.handleCommand(QStringLiteral("reload"));
    QCOMPARE(daemon.modes().modes().size(), 3);
}

void DaemonTest::snippetReproducesTheLiveSetpoints()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    daemon.handleCommand(QStringLiteral("set pl1=17 pl2=26 profile=quiet"));

    const QString reply = daemon.handleCommand(QStringLiteral("snippet tuned"));
    QVERIFY(reply.startsWith(QStringLiteral("ok ")));
    const QString snippet = QString::fromUtf8(QByteArray::fromBase64(reply.mid(3).toUtf8()));
    QVERIFY(snippet.contains(QStringLiteral("[mode tuned]")));
    QVERIFY(snippet.contains(QStringLiteral("pl1_w=17")));
    QVERIFY(snippet.contains(QStringLiteral("pl2_w=26")));
    QVERIFY(snippet.contains(QStringLiteral("platform_profile=quiet")));
    // Nothing live on the backlight side, so the snippet says nothing about it.
    QVERIFY(!snippet.contains(QStringLiteral("[brightness")));

    // One file holds both kinds of section, so one snippet is one paste.
    daemon.handleCommand(QStringLiteral("brightness set span_pct=30 floor_pct=10"));
    const QString both = QString::fromUtf8(QByteArray::fromBase64(
            daemon.handleCommand(QStringLiteral("snippet tuned")).mid(3).toUtf8()));
    QVERIFY(both.contains(QStringLiteral("[mode tuned]")));
    QVERIFY(both.contains(QStringLiteral("[brightness indoors]")));
    QVERIFY(both.contains(QStringLiteral("span_pct=30")));
    QVERIFY(both.contains(QStringLiteral("floor_pct=10")));
    // The curve is not tunable here, and a section without it could not be
    // pasted into the file.
    QVERIFY(both.contains(QStringLiteral("curve=0:0")));
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
    setLight(6000);
    for (int i = 0; i < kBrightnessDwellTicks - 1; ++i)
        daemon.tick();
    QVERIFY(!daemon.fading());
    setLight(1000);
    daemon.tick();
    QCOMPARE(backlightRaw(), seeded);

    // A change that holds moves once the dwell has passed.
    setLight(6000);
    QCOMPARE(tickUntilMove(daemon, kBrightnessDwellTicks + 2), kBrightnessDwellTicks);
    finishFade(daemon);
    QVERIFY(backlightRaw() > seeded);

    // And lands inside the window around the level the person set.
    daemon.tick();
    const Sample &s = daemon.lastSample();
    QCOMPARE(s.brightnessBase, uint8_t(50));
    // Inside the window the profile allows around that level, whatever the
    // default span happens to be tuned to.
    QVERIFY(s.brightness >= 50 - kBrightnessDefaultSpanPct
            && s.brightness <= 50 + kBrightnessDefaultSpanPct);
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
    // so it becomes the base and the window moves with it.
    setBacklightByHand(FakeSysfs::kBacklightMax * 3 / 4);
    daemon.tick();
    QCOMPARE(daemon.state().brightnessBase, 75);

    const QJsonObject brightness =
            statusOf(daemon).value(QStringLiteral("brightness")).toObject();
    QCOMPARE(brightness.value(QStringLiteral("base")).toInt(), 75);
    // The window moved with the correction, and the ceiling cuts it rather than
    // shifting it — which is why the top is not 75 + span when that overshoots.
    QCOMPARE(brightness.value(QStringLiteral("window_low")).toInt(),
             75 - kBrightnessDefaultSpanPct);
    QCOMPARE(brightness.value(QStringLiteral("window_high")).toInt(),
             std::min(100, 75 + kBrightnessDefaultSpanPct));

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
    QVERIFY(QString::fromUtf8(file.readAll()).contains(QStringLiteral("brightness_base=75")));
}

void DaemonTest::theBoundsConstrainTheAutomationAndNotThePerson()
{
    writeConfig(QStringLiteral("[general]\ndefault_mode=work\n"
                               "default_brightness_profile=capped\n"
                               "[mode work]\npl1_w=20\n"
                               "[brightness capped]\n"
                               "span_pct=10\nfloor_pct=20\nceiling_pct=60\n"
                               "curve=0:0, 1000:100\n"));
    Daemon daemon(options());
    QVERIFY(daemon.start());
    daemon.handleCommand(QStringLiteral("brightness on"));
    setLight(1000);
    daemon.tick();

    // A level set by hand above the ceiling is accepted: the bounds constrain
    // the automation, never the person.
    setBacklightByHand(FakeSysfs::kBacklightMax);
    daemon.tick();
    QCOMPARE(daemon.state().brightnessBase, 100);
    for (int i = 0; i < 20; ++i) {
        daemon.tick();
        finishFade(daemon);
    }
    QCOMPARE(backlightRaw(), FakeSysfs::kBacklightMax);

    // Back to a level inside the bounds, and the next automatic move stays
    // within them.
    setBacklightByHand(FakeSysfs::kBacklightMax * 55 / 100);
    daemon.tick();
    QCOMPARE(daemon.state().brightnessBase, 55);
    setLight(0);
    QVERIFY(tickUntilMove(daemon, kBrightnessDwellTicks + 2) > 0);
    finishFade(daemon);

    daemon.tick();
    const Sample &s = daemon.lastSample();
    QVERIFY(s.brightness >= 20 && s.brightness <= 60);
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
    {
        Daemon daemon(options());
        QVERIFY(daemon.start());
        QCOMPARE(daemon.handleCommand(QStringLiteral("brightness on")), QStringLiteral("ok"));
        QCOMPARE(daemon.handleCommand(QStringLiteral("brightness profile indoors")),
                 QStringLiteral("ok indoors"));
        setBacklightByHand(FakeSysfs::kBacklightMax / 4);
        daemon.tick();
        QCOMPARE(daemon.state().brightnessBase, 25);
    }

    Daemon restarted(options());
    QVERIFY(restarted.start());
    QCOMPARE(restarted.state().autoBrightness, true);
    QCOMPARE(restarted.state().brightnessProfile, QStringLiteral("indoors"));
    QCOMPARE(restarted.state().brightnessBase, 25);

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
           QStringLiteral("brightness profile"), QStringLiteral("brightness profile nowhere"),
           QStringLiteral("brightness set span_pct"),
           QStringLiteral("brightness set span_pct=wide"),
           QStringLiteral("brightness set span_pct=101"),
           QStringLiteral("brightness set span_pct=-1"),
           QStringLiteral("brightness set sideways_pct=10"),
           QStringLiteral("brightness set floor_pct=80 ceiling_pct=60") }) {
        QVERIFY2(daemon.handleCommand(command).startsWith(QStringLiteral("err")),
                 qPrintable(command));
    }
    QCOMPARE(daemon.state().autoBrightness, false);

    // A refused command changes nothing, so the profile's own numbers are
    // still the ones in force.
    const QJsonObject brightness =
            statusOf(daemon).value(QStringLiteral("brightness")).toObject();
    QCOMPARE(brightness.value(QStringLiteral("live")).toBool(), false);
    QCOMPARE(brightness.value(QStringLiteral("span_pct")).toInt(), kBrightnessDefaultSpanPct);
}

void DaemonTest::theBrightnessWindowIsTunedLive()
{
    Daemon daemon(options());
    QVERIFY(daemon.start());
    daemon.handleCommand(QStringLiteral("brightness on"));
    setBacklightByHand(FakeSysfs::kBacklightMax / 2);
    daemon.tick();
    QCOMPARE(daemon.state().brightnessBase, 50);

    // As text, because the pair is only ever compared as a whole here.
    const auto window = [&daemon] {
        const QJsonObject brightness =
                statusOf(daemon).value(QStringLiteral("brightness")).toObject();
        return QStringLiteral("%1..%2")
                .arg(brightness.value(QStringLiteral("window_low")).toInt())
                .arg(brightness.value(QStringLiteral("window_high")).toInt());
    };
    const QString profileWindow = QStringLiteral("%1..%2")
                                          .arg(50 - kBrightnessDefaultSpanPct)
                                          .arg(50 + kBrightnessDefaultSpanPct);
    QCOMPARE(window(), profileWindow);

    // Widening it further is a judgement made by eye rather than an edit to the
    // file.
    QCOMPARE(daemon.handleCommand(QStringLiteral("brightness set span_pct=35")),
             QStringLiteral("ok"));
    QCOMPARE(window(), QStringLiteral("15..85"));

    // Cut by the bounds, not shifted by them.
    QCOMPARE(daemon.handleCommand(QStringLiteral("brightness set floor_pct=35 ceiling_pct=70")),
             QStringLiteral("ok"));
    QCOMPARE(window(), QStringLiteral("35..70"));

    // Every live value is in the record, because the profile index alone does
    // not say what the automation was allowed to do.
    daemon.tick();
    const Sample &s = daemon.lastSample();
    QCOMPARE(s.brightnessSpan, uint8_t(35));
    QCOMPARE(s.brightnessFloor, uint8_t(35));
    QCOMPARE(s.brightnessCeiling, uint8_t(70));
    QVERIFY(exporter::csvRow(s).contains(QStringLiteral(",35,35,70")));

    QJsonObject brightness = statusOf(daemon).value(QStringLiteral("brightness")).toObject();
    QCOMPARE(brightness.value(QStringLiteral("live")).toBool(), true);

    // Nothing was written back: the file still says what it said.
    QCOMPARE(daemon.handleCommand(QStringLiteral("brightness reset")), QStringLiteral("ok"));
    QCOMPARE(window(), profileWindow);
    brightness = statusOf(daemon).value(QStringLiteral("brightness")).toObject();
    QCOMPARE(brightness.value(QStringLiteral("live")).toBool(), false);
}

void DaemonTest::theBrightnessOverlayBelongsToItsProfile()
{
    writeConfig(QStringLiteral("[general]\ndefault_mode=work\n"
                               "default_brightness_profile=indoors\n"
                               "[mode work]\npl1_w=20\n"
                               "[brightness indoors]\n"
                               "span_pct=10\nfloor_pct=5\nceiling_pct=100\n"
                               "curve=0:0, 1000:100\n"
                               "[brightness outdoors]\n"
                               "span_pct=15\nfloor_pct=30\nceiling_pct=100\n"
                               "curve=0:0, 1000:100\n"));
    Daemon daemon(options());
    QVERIFY(daemon.start());

    const auto span = [&daemon] {
        return statusOf(daemon)
                .value(QStringLiteral("brightness"))
                .toObject()
                .value(QStringLiteral("span_pct"))
                .toInt();
    };

    QCOMPARE(daemon.handleCommand(QStringLiteral("brightness set span_pct=40")),
             QStringLiteral("ok"));
    QCOMPARE(span(), 40);

    // Selecting another profile drops it: the overlay was an overlay on the
    // profile being left behind.
    QCOMPARE(daemon.handleCommand(QStringLiteral("brightness profile outdoors")),
             QStringLiteral("ok outdoors"));
    QCOMPARE(span(), 15);

    // And so does rereading the file, which is the source of truth.
    QCOMPARE(daemon.handleCommand(QStringLiteral("brightness set span_pct=40")),
             QStringLiteral("ok"));
    QCOMPARE(span(), 40);
    QCOMPARE(daemon.handleCommand(QStringLiteral("reload")), QStringLiteral("ok"));
    QCOMPARE(span(), 15);
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
    QCOMPARE(s.light, kAbsentValue);
    QCOMPARE(m_tree->read(FakeSysfs::raplPackage()
                          + QStringLiteral("/constraint_0_power_limit_uw")),
             QStringLiteral("20000000"));

    const QJsonObject brightness =
            statusOf(daemon).value(QStringLiteral("brightness")).toObject();
    QCOMPARE(brightness.value(QStringLiteral("available")).toBool(), false);
}

QTEST_MAIN(DaemonTest)
#include "daemon_test.moc"
