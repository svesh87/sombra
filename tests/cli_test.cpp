// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "fake_bus.h"
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
    // Waited for with the event loop turning, not by blocking on it. What the child
    // needs may be in this process: a command reaching the daemon is authorised by
    // something standing in for polkit here, and a test asleep in `waitForFinished`
    // would never answer it — three processes each waiting for the next.
    QDeadlineTimer deadline(timeoutMs);
    while (process.state() != QProcess::NotRunning && !deadline.hasExpired())
        QTest::qWait(10);
    if (process.state() != QProcess::NotRunning) {
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
                              QStringLiteral("--state"), work.path() + QStringLiteral("/state"),
                              QStringLiteral("--ticks"), QStringLiteral("1") });
    QCOMPARE(refused.code, 1);
    QVERIFY(refused.err.contains(QStringLiteral("no thermal control nodes")));

    FakeSysfs tree;
    const Run degraded =
            run(daemonBinary(),
                { QStringLiteral("--no-dbus"), QStringLiteral("--root"), tree.root(),
                  QStringLiteral("--ring"), QStringLiteral("/proc/self/impossible.ring"),
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
    PrivateBus bus;
    QString error;
    QVERIFY2(bus.start(&error), qPrintable(error));
    // Something has to answer for polkit or every command would be refused. That the
    // refusal works is control_bus_test's case; this one is about the words.
    FakeAuthority authority;
    QVERIFY2(authority.start(bus.address(), &error), qPrintable(error));

    TestDaemon daemon(tree.root(), work.path(), bus.address());
    QVERIFY2(daemon.waitUntilAnswering(&error), qPrintable(error));

    const QStringList where { QStringLiteral("--bus-address"), bus.address() };
    const Run status = run(ctlBinary(), where + QStringList { QStringLiteral("status") });
    QCOMPARE(status.code, 0);
    // The payload alone, with no framing in front of it: the answer has to go straight
    // into a JSON parser.
    QVERIFY(status.out.startsWith(QStringLiteral("{")));
    QJsonParseError parseError {};
    const QJsonObject parsed =
            QJsonDocument::fromJson(status.out.trimmed().toUtf8(), &parseError).object();
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QCOMPARE(parsed.value(QStringLiteral("mode")).toString(), QStringLiteral("work"));
    // The whole shape, not only the one field: this output is an interface, and
    // scripts and the tuning analysis read it.
    for (const char *key : { "ts", "modes", "applied", "requested", "sensors", "brightness",
                             "monitors", "metrics" }) {
        QVERIFY2(parsed.contains(QLatin1String(key)), key);
    }

    QCOMPARE(run(ctlBinary(), where + QStringList { QStringLiteral("mode"),
                                                   QStringLiteral("quiet") })
                     .code,
             0);
    QCOMPARE(run(ctlBinary(), where + QStringList { QStringLiteral("set"),
                                                    QStringLiteral("pl1=14") })
                     .code,
             0);
    QCOMPARE(run(ctlBinary(), where + QStringList { QStringLiteral("mark"),
                                                    QStringLiteral("good") })
                     .code,
             0);

    // Keeping a live value is a named command with a named destination, and what it
    // wrote is readable back through the same tool.
    QCOMPARE(run(ctlBinary(), where + QStringList { QStringLiteral("config"),
                                                    QStringLiteral("save"),
                                                    QStringLiteral("live"),
                                                    QStringLiteral("to"),
                                                    QStringLiteral("quiet") })
                     .code,
             0);
    const Run shown = run(ctlBinary(), where + QStringList { QStringLiteral("config"),
                                                             QStringLiteral("show") });
    QCOMPARE(shown.code, 0);
    QVERIFY(shown.out.startsWith(QStringLiteral("{")));
    const QJsonObject configuration =
            QJsonDocument::fromJson(shown.out.trimmed().toUtf8()).object();
    QVERIFY(configuration.value(QStringLiteral("stored")).toBool());
    const QJsonArray modes = configuration.value(QStringLiteral("modes")).toArray();
    QCOMPARE(modes.size(), 3);
    bool sawTheSavedValue = false;
    for (const QJsonValue &entry : modes) {
        if (entry.toObject().value(QStringLiteral("id")).toString() == QLatin1String("quiet"))
            sawTheSavedValue = entry.toObject().value(QStringLiteral("pl1_w")).toInt() == 14;
    }
    QVERIFY(sawTheSavedValue);

    // The monitors are a reading rather than a call, and they print as JSON too.
    const Run monitors = run(ctlBinary(), where + QStringList { QStringLiteral("monitor"),
                                                                QStringLiteral("list") });
    QCOMPARE(monitors.code, 0);
    QVERIFY(monitors.out.startsWith(QStringLiteral("[")));

    // A word the daemon does not know is refused by the tool, before a bus is
    // touched at all, and a command with no words is a usage error.
    QCOMPARE(run(ctlBinary(), where + QStringList { QStringLiteral("levitate") }).code, 2);
    QCOMPARE(run(ctlBinary(), where).code, 2);
    // And one it knows but refuses is an exit code with the reason on the line.
    const Run refused = run(ctlBinary(), where + QStringList { QStringLiteral("mode"),
                                                               QStringLiteral("teapot") });
    QCOMPARE(refused.code, 1);
    QVERIFY(refused.out.startsWith(QStringLiteral("err")));

    daemon.stop();
    QCOMPARE(tree.read(FakeSysfs::asus() + QStringLiteral("/pwm1_enable")), QStringLiteral("2"));
    QCOMPARE(tree.read(FakeSysfs::raplPackage()
                       + QStringLiteral("/constraint_0_power_limit_uw")),
             QStringLiteral("45000000"));
}

void CliTest::ctlReportsAnUnreachableDaemon()
{
    // A bus with nothing on it. The tool says which name went unanswered rather than
    // hanging or printing an empty object.
    PrivateBus bus;
    QString error;
    QVERIFY2(bus.start(&error), qPrintable(error));
    const Run result = run(ctlBinary(), { QStringLiteral("--bus-address"), bus.address(),
                                          QStringLiteral("status") });
    QCOMPARE(result.code, 1);
    QVERIFY(result.err.contains(QStringLiteral("not answering")));
    QVERIFY(result.err.contains(QStringLiteral("io.github.svesh87.Sombra")));
}

void CliTest::exportRendersTheRingInBothFormats()
{
    FakeSysfs tree;
    QTemporaryDir work;
    const QString ring = work.path() + QStringLiteral("/metrics.ring");
    QCOMPARE(run(daemonBinary(),
                 { QStringLiteral("--no-dbus"), QStringLiteral("--root"), tree.root(),
                   QStringLiteral("--ring"), ring,
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
    QVERIFY(!help.out.contains(QStringLiteral("--bus-address")));
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
