// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
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

// The daemon runs as its own process here, which is what it is on the machine
// as well. It cannot be an object in this process: the client blocks on the
// socket waiting for its reply, and a daemon sharing the thread would never
// reach its own event loop to answer.
class TestDaemon {
public:
    TestDaemon(const QString &sysfsRoot, QString workDir)
        : m_work(std::move(workDir))
    {
        m_process.start(binary(),
                        { QStringLiteral("--no-dbus"), QStringLiteral("--root"), sysfsRoot,
                          QStringLiteral("--ring"), m_work + QStringLiteral("/metrics.ring"),
                          QStringLiteral("--socket"), socketPath(), QStringLiteral("--state"),
                          m_work + QStringLiteral("/state"), QStringLiteral("--config"),
                          m_work + QStringLiteral("/modes.conf"), QStringLiteral("--capacity"),
                          QStringLiteral("64"), QStringLiteral("--interval-ms"),
                          QStringLiteral("20") });
    }

    ~TestDaemon() { stop(); }

    QString socketPath() const { return m_work + QStringLiteral("/control.sock"); }

    bool waitUntilListening()
    {
        if (!m_process.waitForStarted(10000))
            return false;
        QDeadlineTimer deadline(10000);
        while (!QFile::exists(socketPath()) && !deadline.hasExpired())
            QTest::qWait(10);
        return QFile::exists(socketPath());
    }

    // SIGTERM is how systemd stops it, and it restores the hardware and
    // removes the socket on the way out.
    void stop()
    {
        if (m_process.state() == QProcess::NotRunning)
            return;
        m_process.terminate();
        m_process.waitForFinished(10000);
    }

    static QString binary()
    {
        return QDir::cleanPath(QCoreApplication::applicationDirPath()
                               + QStringLiteral("/../ombrad"));
    }

private:
    QString m_work;
    QProcess m_process;
};

QJsonObject objectFrom(const QByteArray &json)
{
    return QJsonDocument::fromJson(json).object();
}

} // namespace

class TrayClientTest : public QObject {
    Q_OBJECT

private slots:
    void init();

    void parsesAFullStatus();
    void parsesAbsentReadingsAsTheSentinel();
    void parsesMalformedRepliesWithoutCrashing();
    void pollsARunningDaemonAndReconnectsToANewOne();
    void sendReturnsNothingWithoutADaemon();
    void sendAndRefreshDrivesTheDaemonAndRepaints();

private:
    QString socketPath() const { return m_work.path() + QStringLiteral("/control.sock"); }

    QTemporaryDir m_work;
    // Rebuilt before every test: a shared tree would carry one case's writes
    // into the next.
    std::unique_ptr<FakeSysfs> m_tree;
};

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
                    "gpu_mhz": 300, "load1": 0.61}
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
}

void TrayClientTest::pollsARunningDaemonAndReconnectsToANewOne()
{
    auto daemon = std::make_unique<TestDaemon>(m_tree->root(), m_work.path());
    QVERIFY(daemon->waitUntilListening());

    TrayClient client(socketPath());
    int updates = 0;
    connect(&client, &TrayClient::statusChanged, this,
            [&updates](const TrayClient::Status &) { ++updates; });
    QSignalSpy connectionSpy(&client, &TrayClient::connectionChanged);

    client.start(20);
    QTRY_VERIFY(updates >= 2);
    QVERIFY(client.connected());
    QCOMPARE(connectionSpy.count(), 1);
    QCOMPARE(connectionSpy.first().first().toBool(), true);
    QCOMPARE(client.status().modeId, QStringLiteral("work"));
    QCOMPARE(client.status().requested.pl1W, 20);
    QCOMPARE(client.status().packageC, 61.0);
    QCOMPARE(client.socketPath(), socketPath());

    // The daemon may restart under a running tray; the tray does not.
    daemon.reset();
    QTRY_VERIFY(!client.connected());
    QCOMPARE(connectionSpy.count(), 2);
    QCOMPARE(connectionSpy.last().first().toBool(), false);

    TestDaemon replacement(m_tree->root(), m_work.path());
    QVERIFY(replacement.waitUntilListening());
    QTRY_VERIFY(client.connected());
    QCOMPARE(connectionSpy.count(), 3);
    QCOMPARE(client.status().modeId, QStringLiteral("work"));

    client.stop();
    const int settled = updates;
    QTest::qWait(100);
    QCOMPARE(updates, settled);
}

void TrayClientTest::sendReturnsNothingWithoutADaemon()
{
    TrayClient client(m_work.path() + QStringLiteral("/absent.sock"));
    QVERIFY(client.send(QStringLiteral("status")).isEmpty());
    QVERIFY(!client.connected());

    QSignalSpy connectionSpy(&client, &TrayClient::connectionChanged);
    client.poll();
    QVERIFY(!client.status().valid);
    // Nothing changed, so nothing is announced: the interface is not repainted
    // once a second while the daemon is absent.
    QCOMPARE(connectionSpy.count(), 0);
}

void TrayClientTest::sendAndRefreshDrivesTheDaemonAndRepaints()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());

    TrayClient client(socketPath());
    int updates = 0;
    connect(&client, &TrayClient::statusChanged, this,
            [&updates](const TrayClient::Status &) { ++updates; });

    QCOMPARE(client.sendAndRefresh(QStringLiteral("mode quiet")), QStringLiteral("ok quiet"));
    QCOMPARE(updates, 1);
    QCOMPARE(client.status().modeId, QStringLiteral("quiet"));

    // Live tuning applies at once and shows up in the very next status.
    QCOMPARE(client.sendAndRefresh(QStringLiteral("set pl1=15")), QStringLiteral("ok"));
    QVERIFY(client.status().liveTuning);
    QCOMPARE(client.status().requested.pl1W, 15);

    QVERIFY(client.send(QStringLiteral("levitate")).startsWith(QStringLiteral("err")));
}

QTEST_MAIN(TrayClientTest)
#include "tray_client_test.moc"
