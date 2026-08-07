// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "fake_sysfs.h"

#include <QDeadlineTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

// End-to-end: the real binaries, against a fake control surface. This is also
// the only place the option parsing in the main functions is exercised.
class CliTest : public QObject {
    Q_OBJECT

private slots:
    void daemonRefusesATreeItCannotManage();
    void daemonAppliesAModeAndExitsRestoring();
    void ctlDrivesTheRunningDaemon();
    void ctlReportsAnUnreachableDaemon();
    void exportRendersTheRingInBothFormats();
    void exportRejectsBadInput();
    void helpAndVersionWork();

private:
    static QString daemonBinary() { return QStringLiteral(DAEMON_BINARY); }
    static QString ctlBinary() { return QStringLiteral(CTL_BINARY); }

    struct Run {
        int code = -1;
        QString out;
        QString err;
    };
    static Run run(const QString &program, const QStringList &arguments, int timeoutMs = 15000);
};

CliTest::Run CliTest::run(const QString &program, const QStringList &arguments, int timeoutMs)
{
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(5000))
        return {};
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        return {};
    }
    Run result;
    result.code = process.exitCode();
    result.out = QString::fromUtf8(process.readAllStandardOutput());
    result.err = QString::fromUtf8(process.readAllStandardError());
    return result;
}

void CliTest::daemonRefusesATreeItCannotManage()
{
    QTemporaryDir bare;
    QTemporaryDir work;
    // A tree with no control nodes is the one startup condition that is fatal.
    // A ring that cannot be created is not: telemetry is secondary to keeping
    // the machine inside its thermal envelope.
    const Run refused = run(daemonBinary(),
                            { QStringLiteral("--no-dbus"), QStringLiteral("--root"), bare.path(),
                              QStringLiteral("--ring"),
                              work.path() + QStringLiteral("/metrics.ring"),
                              QStringLiteral("--socket"), work.path() + QStringLiteral("/c.sock"),
                              QStringLiteral("--state"), work.path() + QStringLiteral("/state"),
                              QStringLiteral("--ticks"), QStringLiteral("1") });
    QCOMPARE(refused.code, 1);
    QVERIFY(refused.err.contains(QStringLiteral("no thermal control nodes")));

    FakeSysfs tree;
    const Run degraded =
            run(daemonBinary(),
                { QStringLiteral("--no-dbus"), QStringLiteral("--root"), tree.root(),
                  QStringLiteral("--ring"), QStringLiteral("/proc/self/impossible.ring"),
                  QStringLiteral("--socket"), work.path() + QStringLiteral("/c.sock"),
                  QStringLiteral("--state"), work.path() + QStringLiteral("/state"),
                  QStringLiteral("--config"), work.path() + QStringLiteral("/none.conf"),
                  QStringLiteral("--interval-ms"), QStringLiteral("10"),
                  QStringLiteral("--ticks"), QStringLiteral("2") });
    QCOMPARE(degraded.code, 0);
    QVERIFY(degraded.err.contains(QStringLiteral("telemetry disabled")));
    QCOMPARE(tree.read(FakeSysfs::pstate() + QStringLiteral("/max_perf_pct")),
             QStringLiteral("100"));
}

void CliTest::daemonAppliesAModeAndExitsRestoring()
{
    FakeSysfs tree;
    QTemporaryDir work;
    const Run result = run(daemonBinary(),
                           { QStringLiteral("--no-dbus"), QStringLiteral("--root"), tree.root(),
                             QStringLiteral("--ring"),
                             work.path() + QStringLiteral("/metrics.ring"),
                             QStringLiteral("--socket"), work.path() + QStringLiteral("/c.sock"),
                             QStringLiteral("--state"), work.path() + QStringLiteral("/state"),
                             QStringLiteral("--config"), work.path() + QStringLiteral("/none.conf"),
                             QStringLiteral("--capacity"), QStringLiteral("0"),
                             QStringLiteral("--interval-ms"), QStringLiteral("0"),
                             QStringLiteral("--ticks"), QStringLiteral("2") });
    QCOMPARE(result.code, 0);

    // Ran, then left the machine in the confirmed-safe state on the way out.
    QCOMPARE(tree.read(FakeSysfs::asus() + QStringLiteral("/pwm1_enable")), QStringLiteral("2"));
    QVERIFY(QFile::exists(work.path() + QStringLiteral("/metrics.ring")));
    QFile state(work.path() + QStringLiteral("/state"));
    QVERIFY(state.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString saved = QString::fromUtf8(state.readAll());
    QVERIFY(saved.contains(QStringLiteral("mode=work")));
    // The automation is off by default and stays off, so a first run must not
    // have written it on.
    QVERIFY(saved.contains(QStringLiteral("auto_brightness=off")));
}

void CliTest::ctlDrivesTheRunningDaemon()
{
    FakeSysfs tree;
    QTemporaryDir work;
    const QString socket = work.path() + QStringLiteral("/control.sock");

    QProcess daemon;
    daemon.start(daemonBinary(),
                 { QStringLiteral("--no-dbus"), QStringLiteral("--root"), tree.root(),
                   QStringLiteral("--ring"),
                   work.path() + QStringLiteral("/metrics.ring"), QStringLiteral("--socket"),
                   socket, QStringLiteral("--state"), work.path() + QStringLiteral("/state"),
                   QStringLiteral("--config"), work.path() + QStringLiteral("/none.conf"),
                   QStringLiteral("--capacity"), QStringLiteral("128"),
                   QStringLiteral("--interval-ms"), QStringLiteral("20") });
    QVERIFY(daemon.waitForStarted(5000));

    QDeadlineTimer deadline(10000);
    while (!QFile::exists(socket) && !deadline.hasExpired())
        QTest::qWait(20);
    QVERIFY(QFile::exists(socket));

    const Run status = run(ctlBinary(), { QStringLiteral("--socket"), socket,
                                          QStringLiteral("status") });
    QCOMPARE(status.code, 0);
    QVERIFY(status.out.startsWith(QStringLiteral("ok {")));
    const QJsonObject parsed =
            QJsonDocument::fromJson(status.out.trimmed().mid(3).toUtf8()).object();
    QCOMPARE(parsed.value(QStringLiteral("mode")).toString(), QStringLiteral("work"));

    QCOMPARE(run(ctlBinary(), { QStringLiteral("--socket"), socket, QStringLiteral("mode"),
                                QStringLiteral("quiet") })
                     .code,
             0);
    QCOMPARE(run(ctlBinary(), { QStringLiteral("--socket"), socket, QStringLiteral("set"),
                                QStringLiteral("pl1=14") })
                     .code,
             0);
    QCOMPARE(run(ctlBinary(), { QStringLiteral("--socket"), socket, QStringLiteral("mark"),
                                QStringLiteral("good") })
                     .code,
             0);
    // A snippet is multi-line, so it travels base64-encoded and is unpacked
    // by the client rather than by every caller.
    const Run snippet = run(ctlBinary(), { QStringLiteral("--socket"), socket,
                                           QStringLiteral("snippet"), QStringLiteral("tuned") });
    QCOMPARE(snippet.code, 0);
    QVERIFY(snippet.out.contains(QStringLiteral("[mode tuned]")));
    QVERIFY(snippet.out.contains(QStringLiteral("pl1_w=14")));

    // A rejected command is a non-zero exit, so a script can branch on it.
    QCOMPARE(run(ctlBinary(), { QStringLiteral("--socket"), socket, QStringLiteral("levitate") })
                     .code,
             1);
    QCOMPARE(run(ctlBinary(), { QStringLiteral("--socket"), socket }).code, 2);

    deadline = QDeadlineTimer(5000);
    while (tree.read(FakeSysfs::platformProfile()) != QStringLiteral("quiet")
           && !deadline.hasExpired())
        QTest::qWait(20);
    QCOMPARE(tree.read(FakeSysfs::platformProfile()), QStringLiteral("quiet"));

    // SIGTERM is how systemd stops it, and it has to end with the fan back on
    // EC control.
    daemon.terminate();
    QVERIFY(daemon.waitForFinished(10000));
    QCOMPARE(tree.read(FakeSysfs::asus() + QStringLiteral("/pwm1_enable")), QStringLiteral("2"));
    QCOMPARE(tree.read(FakeSysfs::raplPackage()
                       + QStringLiteral("/constraint_0_power_limit_uw")),
             QStringLiteral("45000000"));
}

void CliTest::ctlReportsAnUnreachableDaemon()
{
    QTemporaryDir work;
    const Run result = run(ctlBinary(), { QStringLiteral("--socket"),
                                          work.path() + QStringLiteral("/absent.sock"),
                                          QStringLiteral("status") });
    QCOMPARE(result.code, 1);
    QVERIFY(result.err.contains(QStringLiteral("cannot reach")));
}

void CliTest::exportRendersTheRingInBothFormats()
{
    FakeSysfs tree;
    QTemporaryDir work;
    const QString ring = work.path() + QStringLiteral("/metrics.ring");
    QCOMPARE(run(daemonBinary(),
                 { QStringLiteral("--no-dbus"), QStringLiteral("--root"), tree.root(),
                   QStringLiteral("--ring"), ring,
                   QStringLiteral("--socket"), work.path() + QStringLiteral("/c.sock"),
                   QStringLiteral("--state"), work.path() + QStringLiteral("/state"),
                   QStringLiteral("--config"), work.path() + QStringLiteral("/none.conf"),
                   QStringLiteral("--capacity"), QStringLiteral("64"),
                   QStringLiteral("--interval-ms"), QStringLiteral("10"),
                   QStringLiteral("--ticks"), QStringLiteral("3") })
                     .code,
             0);

    // Reading the ring is the one command that does not go to the daemon, so
    // it works with nothing listening.
    const Run csv = run(ctlBinary(), { QStringLiteral("export"), QStringLiteral("--ring"), ring });
    QCOMPARE(csv.code, 0);
    const QStringList lines = csv.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 4);
    QVERIFY(lines.first().startsWith(QStringLiteral("ts,pkg_c")));

    const Run json = run(ctlBinary(), { QStringLiteral("export"), QStringLiteral("--ring"), ring,
                                        QStringLiteral("--format"), QStringLiteral("json") });
    QCOMPARE(json.code, 0);
    QCOMPARE(QJsonDocument::fromJson(json.out.toUtf8()).array().size(), 3);

    const Run windowed =
            run(ctlBinary(), { QStringLiteral("export"), QStringLiteral("--ring"), ring,
                               QStringLiteral("--from"), QStringLiteral("4000000000") });
    QCOMPARE(windowed.code, 0);
    QCOMPARE(windowed.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts).size(), 1);

    // The subcommand has its own options and its own help, which is why it is
    // dispatched before the control parser sees the arguments.
    const Run help = run(ctlBinary(), { QStringLiteral("export"), QStringLiteral("--help") });
    QCOMPARE(help.code, 0);
    QVERIFY(help.out.contains(QStringLiteral("--ring")));
    QVERIFY(!help.out.contains(QStringLiteral("--socket")));
}

void CliTest::exportRejectsBadInput()
{
    QTemporaryDir work;
    QCOMPARE(run(ctlBinary(), { QStringLiteral("export"), QStringLiteral("--ring"),
                                work.path() + QStringLiteral("/absent.ring") })
                     .code,
             1);
    QCOMPARE(run(ctlBinary(), { QStringLiteral("export"), QStringLiteral("--format"),
                                QStringLiteral("yaml") })
                     .code,
             2);
}

void CliTest::helpAndVersionWork()
{
    for (const QString &binary : { daemonBinary(), ctlBinary() }) {
        const Run help = run(binary, { QStringLiteral("--help") });
        QCOMPARE(help.code, 0);
        QVERIFY2(!help.out.isEmpty(), qPrintable(binary));
        QCOMPARE(run(binary, { QStringLiteral("--version") }).code, 0);
        QCOMPARE(run(binary, { QStringLiteral("--nonsense") }).code, 1);
    }
}

QTEST_MAIN(CliTest)
#include "cli_test.moc"
