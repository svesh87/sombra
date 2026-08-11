// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "fake_bus.h"
#include "fake_sysfs.h"
#include "tray_client.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

namespace {

QJsonObject objectFrom(const QByteArray &json)
{
    return QJsonDocument::fromJson(json).object();
}

} // namespace

class TrayClientTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void parsesAFullStatus();
    void parsesAbsentReadingsAsTheSentinel();
    void parsesMalformedRepliesWithoutCrashing();
    void followsARunningDaemonAndNoticesANewOne();
    void answersWithoutADaemonInsteadOfWaiting();
    void sendAndRefreshDrivesTheDaemonAndRepaints();

private:
    QTemporaryDir m_work;
    // One bus and one authority for the whole case file: what a case owns is the
    // daemon on it.
    PrivateBus m_bus;
    std::unique_ptr<FakeAuthority> m_authority;
    // Rebuilt before every test: a shared tree would carry one case's writes
    // into the next.
    std::unique_ptr<FakeSysfs> m_tree;
};

void TrayClientTest::initTestCase()
{
    QString error;
    QVERIFY2(m_bus.start(&error), qPrintable(error));
    // Without something answering for polkit every command would be refused, which is
    // not what these cases are about — the refusal itself is control_bus_test's.
    m_authority = std::make_unique<FakeAuthority>();
    QVERIFY2(m_authority->start(m_bus.address(), &error), qPrintable(error));
}

void TrayClientTest::init()
{
    m_tree = std::make_unique<FakeSysfs>();
    QFile::remove(m_work.path() + QStringLiteral("/state"));
    QFile::remove(m_work.path() + QStringLiteral("/metrics.ring"));
}

void TrayClientTest::parsesAFullStatus()
{
    const TrayClient::Status status = TrayClient::parseStatus(objectFrom(R"({
        "ts": 1700000000,
        "mode": "work",
        "modes": [{"id": "quiet", "label": "Quiet"}, {"id": "work", "label": "Work"},
                  {"id": "full", "label": "Full"}],
        "live_tuning": true,
        "guard_engaged": true,
        "fan_forced": true,
        "write_mismatch": true,
        "sensors_degraded": true,
        "on_ac": true,
        "charge_limit": 80,
        "ticks": 42,
        "applied": {"profile": "quiet", "pl1_w": 10, "pl2_w": 15, "max_perf_pct": 30,
                    "turbo": false, "epp": "power", "hwp_boost": false, "gt_max_mhz": 1000,
                    "fan_mode": 0},
        "requested": {"profile": "performance", "pl1_w": 45, "pl2_w": 65, "max_perf_pct": 100,
                      "turbo": true, "epp": "balance_performance", "hwp_boost": true,
                      "gt_max_mhz": 2350},
        "sensors": {"pkg_c": 61.0, "board_c": 45.0, "nvme_c": 38.8, "fan_rpm": 1900,
                    "pkg_w": 7.25, "core_w": 4.5, "uncore_w": 1.25, "cpu_mhz": 2400,
                    "gpu_mhz": 300, "load1": 0.61},
        "brightness": {"available": true, "auto": true,
                       "level": 60, "base": 55, "light_lux": 1.5,
                       "response_pct": 30, "floor_pct": 35,
                       "window_low": 35, "window_high": 85}
    })"));

    QVERIFY(status.valid);
    QCOMPARE(status.modeId, QStringLiteral("work"));
    // Ordered coolest first, as the daemon reports them.
    QCOMPARE(status.modeIds, QStringList({ QStringLiteral("quiet"), QStringLiteral("work"),
                                           QStringLiteral("full") }));
    QCOMPARE(status.modeLabels, QStringList({ QStringLiteral("Quiet"), QStringLiteral("Work"),
                                              QStringLiteral("Full") }));
    QVERIFY(status.liveTuning);
    QVERIFY(status.guardEngaged);
    QVERIFY(status.fanForced);
    QVERIFY(status.writeMismatch);
    QVERIFY(status.sensorsDegraded);
    QVERIFY(status.onAc);

    // Applied and requested differ exactly while the guard is clamping, which
    // is the situation this payload describes.
    QCOMPARE(status.applied.profile, Profile::Quiet);
    QCOMPARE(status.applied.pl1W, 10);
    QCOMPARE(status.applied.pl2W, 15);
    QCOMPARE(status.applied.maxPerfPct, 30);
    QVERIFY(status.applied.noTurbo);
    QCOMPARE(status.applied.epp, Epp::Power);
    QVERIFY(!status.applied.hwpBoost);
    QCOMPARE(status.applied.gtMaxMhz, 1000);
    QCOMPARE(status.applied.fanMode, 0);

    QCOMPARE(status.requested.profile, Profile::Performance);
    QCOMPARE(status.requested.pl1W, kMaxConfirmedPl1W);
    QCOMPARE(status.requested.pl2W, kMaxConfirmedPl2W);
    QCOMPARE(status.requested.maxPerfPct, 100);
    QVERIFY(!status.requested.noTurbo);
    QCOMPARE(status.requested.epp, Epp::BalancePerformance);
    QVERIFY(status.requested.hwpBoost);
    QCOMPARE(status.requested.gtMaxMhz, kMaxGtMhz);
    // Only the applied setpoints carry a fan mode; a mode cannot ask for one.
    QCOMPARE(status.requested.fanMode, 2);

    QCOMPARE(status.packageC, 61.0);
    QCOMPARE(status.boardC, 45.0);
    QCOMPARE(status.nvmeC, 38.8);
    QCOMPARE(status.fanRpm, 1900.0);
    QCOMPARE(status.packageW, 7.25);
    // The core and uncore split is what tells CPU heat from iGPU heat.
    QCOMPARE(status.coreW, 4.5);
    QCOMPARE(status.uncoreW, 1.25);
    QCOMPARE(status.cpuMhz, 2400.0);
    QCOMPARE(status.gpuMhz, 300.0);
    QCOMPARE(status.load1, 0.61);

    // The backlight side, reported alongside the thermal state and never mixed
    // with it. The sensor reading is deliberately not carried across: the
    // console shows per cent and nothing else.
    QVERIFY(status.brightnessAvailable);
    QVERIFY(status.autoBrightness);
    QCOMPARE(status.brightness, 60.0);
    QCOMPARE(status.brightnessBase, 55.0);
    QCOMPARE(status.brightnessResponsePct, 30);
    QCOMPARE(status.brightnessFloorPct, 35);
    // The band, which is what the window says out loud.
    QCOMPARE(status.brightnessWindowLow, 35);
    QCOMPARE(status.brightnessWindowHigh, 85);
}

void TrayClientTest::parsesAbsentReadingsAsTheSentinel()
{
    const TrayClient::Status status = TrayClient::parseStatus(objectFrom(R"({
        "mode": "quiet",
        "modes": [{"id": "quiet", "label": "Quiet"}],
        "applied": {"profile": "quiet", "pl1_w": 12, "pl2_w": 20, "max_perf_pct": 40,
                    "turbo": false, "epp": "power", "hwp_boost": false, "gt_max_mhz": 1000,
                    "fan_mode": 2},
        "requested": {"profile": "quiet", "pl1_w": 12, "pl2_w": 20, "max_perf_pct": 40,
                      "turbo": false, "epp": "power", "hwp_boost": false, "gt_max_mhz": 1000},
        "sensors": {"pkg_c": null, "board_c": null, "nvme_c": null, "fan_rpm": null,
                    "pkg_w": null, "core_w": null, "uncore_w": null, "cpu_mhz": null,
                    "gpu_mhz": null, "load1": null}
    })"));

    QVERIFY(status.valid);
    // A reading the daemon could not take keeps the sentinel: zero is a legal
    // fan speed and a legal power draw, so it cannot stand in for "absent".
    for (const double value : { status.packageC, status.boardC, status.nvmeC, status.fanRpm,
                                status.packageW, status.coreW, status.uncoreW, status.cpuMhz,
                                status.gpuMhz, status.load1 }) {
        QCOMPARE(value, TrayClient::kNoReading);
    }
}

void TrayClientTest::parsesMalformedRepliesWithoutCrashing()
{
    const TrayClient::Status empty = TrayClient::parseStatus(objectFrom("{ this is not json"));
    QVERIFY(!empty.valid);
    QVERIFY(empty.modeIds.isEmpty());
    QCOMPARE(empty.packageC, TrayClient::kNoReading);

    // Present but nonsensical: the setpoints keep their defaults rather than
    // becoming an unknown profile the interface cannot render.
    const TrayClient::Status garbage = TrayClient::parseStatus(objectFrom(R"({
        "mode": 17,
        "modes": "not an array",
        "applied": {"profile": "sideways", "epp": "sideways"},
        "requested": [],
        "sensors": {"pkg_c": "warm"}
    })"));
    QVERIFY(garbage.valid);
    QVERIFY(garbage.modeIds.isEmpty());
    QCOMPARE(garbage.applied.profile, Setpoints().profile);
    QCOMPARE(garbage.applied.epp, Setpoints().epp);
    QCOMPARE(garbage.applied.pl1W, Setpoints().pl1W);
    QCOMPARE(garbage.packageC, TrayClient::kNoReading);
    // Without a brightness object the sliders still have somewhere to sit.
    QCOMPARE(garbage.brightnessResponsePct, kBrightnessDefaultResponsePct);
    QCOMPARE(garbage.brightnessFloorPct, kBrightnessDefaultFloorPct);
}

void TrayClientTest::followsARunningDaemonAndNoticesANewOne()
{
    auto daemon = std::make_unique<TestDaemon>(m_tree->root(), m_work.path(), m_bus.address());
    QString error;
    QVERIFY2(daemon->waitUntilAnswering(&error), qPrintable(error));

    TrayClient client(m_bus.address());
    int updates = 0;
    connect(&client, &TrayClient::statusChanged, this,
            [&updates](const TrayClient::Status &) { ++updates; });
    QSignalSpy connectionSpy(&client, &TrayClient::connectionChanged);

    client.start();
    QVERIFY(client.connected());
    QCOMPARE(connectionSpy.count(), 1);
    QCOMPARE(connectionSpy.first().first().toBool(), true);
    QCOMPARE(client.status().modeId, QStringLiteral("work"));
    QCOMPARE(client.status().requested.pl1W, 20);

    // Nothing is asked for once a second: the readings arrive because the daemon
    // announces them as it takes them, and the first ones arrive with its first tick.
    QTRY_COMPARE(client.status().packageC, 61.0);
    const int settled = updates;
    QTRY_VERIFY(updates > settled);

    // The daemon may restart under a running console; the console does not. The name
    // changing hands is what says so, and there is no timer looking for it.
    daemon.reset();
    QTRY_VERIFY(!client.connected());
    QCOMPARE(connectionSpy.count(), 2);
    QCOMPARE(connectionSpy.last().first().toBool(), false);

    TestDaemon replacement(m_tree->root(), m_work.path(), m_bus.address());
    QVERIFY2(replacement.waitUntilAnswering(&error), qPrintable(error));
    QTRY_VERIFY(client.connected());
    QCOMPARE(connectionSpy.count(), 3);
    QCOMPARE(client.status().modeId, QStringLiteral("work"));
}

void TrayClientTest::answersWithoutADaemonInsteadOfWaiting()
{
    // A bus with nothing on it, which is what a console started before the daemon
    // sees. Every command comes back at once rather than being waited on, and with
    // nothing to say: what to call that is the window's business, and it has words
    // for it that are about this daemon rather than about D-Bus.
    TrayClient client(m_bus.address());
    QVERIFY(client.send(QStringLiteral("mode quiet")).isEmpty());
    QVERIFY(!client.connected());
    // A command that makes no sense is refused without a daemon being needed for it.
    QVERIFY(client.send(QStringLiteral("levitate")).startsWith(QStringLiteral("err")));

    QSignalSpy connectionSpy(&client, &TrayClient::connectionChanged);
    client.poll();
    QVERIFY(!client.status().valid);
    // Nothing changed, so nothing is announced: the interface is not repainted while
    // the daemon is absent.
    QCOMPARE(connectionSpy.count(), 0);
}

void TrayClientTest::sendAndRefreshDrivesTheDaemonAndRepaints()
{
    TestDaemon daemon(m_tree->root(), m_work.path(), m_bus.address());
    QString error;
    QVERIFY2(daemon.waitUntilAnswering(&error), qPrintable(error));

    TrayClient client(m_bus.address());
    int updates = 0;
    connect(&client, &TrayClient::statusChanged, this,
            [&updates](const TrayClient::Status &) { ++updates; });

    QCOMPARE(client.sendAndRefresh(QStringLiteral("mode quiet")), QStringLiteral("ok quiet"));
    // The window is repainted from the answer rather than at the next tick: the daemon
    // announces the change as it makes it, and the refresh reads it as well.
    QVERIFY(updates > 0);
    QCOMPARE(client.status().modeId, QStringLiteral("quiet"));

    // Live tuning applies at once and shows up in the very next status.
    QCOMPARE(client.sendAndRefresh(QStringLiteral("set pl1=15")), QStringLiteral("ok"));
    QVERIFY(client.status().liveTuning);
    QCOMPARE(client.status().requested.pl1W, 15);

    QVERIFY(client.send(QStringLiteral("levitate")).startsWith(QStringLiteral("err")));
}

QTEST_MAIN(TrayClientTest)
#include "tray_client_test.moc"
