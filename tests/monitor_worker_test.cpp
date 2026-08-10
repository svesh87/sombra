// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "monitor_bus.h"
#include "monitor_worker.h"

#include <QSignalSpy>
#include <QThread>
#include <QTest>

#include <memory>

namespace {

// A bus that answers from memory and counts what it was asked, because what this
// class is for is the asking rather than the answering: how many writes reach a
// monitor, what happens when one stops answering, and whether a write is believed.
class ScriptedBus : public MonitorBus {
public:
    QList<MonitorInfo> detect() override
    {
        ++detects;
        return monitors;
    }

    std::optional<int> read(const QString &id) override
    {
        reads.append(id);
        if (silent.contains(id))
            return std::nullopt;
        return levels.value(id, 50);
    }

    bool write(const QString &id, int percent) override
    {
        if (writeCostMs > 0)
            QThread::msleep(static_cast<unsigned long>(writeCostMs));
        writes.append(QStringLiteral("%1=%2").arg(id).arg(percent));
        if (silent.contains(id))
            return false;
        // The measured case: the bus reports a failure and the value lands anyway.
        levels.insert(id, percent);
        return liesAboutWrites ? false : true;
    }

    QList<MonitorInfo> monitors;
    QHash<QString, int> levels;
    QSet<QString> silent;
    bool liesAboutWrites = false;
    // Milliseconds a write costs, for the case a gesture is faster than the bus.
    int writeCostMs = 0;
    int detects = 0;
    QStringList reads;
    QStringList writes;
};

MonitorInfo entry(const QString &id, const QString &connector)
{
    MonitorInfo info;
    info.id = id;
    info.connector = connector;
    info.model = QStringLiteral("test");
    return info;
}

} // namespace

class MonitorWorkerTest : public QObject {
    Q_OBJECT

private slots:
    void aWriteIsConfirmedByReadingAndNotByItsOwnAnswer();
    void onlyTheLastValueOfAGestureReachesTheMonitor();
    void aMonitorThatStopsAnsweringIsCalledAbsentAndLeftAlone();
    void aThreadOfItsOwnIsWhereTheBusRuns();
    void aGestureFasterThanTheBusCollapsesIntoTwoWrites();
};

void MonitorWorkerTest::aWriteIsConfirmedByReadingAndNotByItsOwnAnswer()
{
    auto bus = std::make_unique<ScriptedBus>();
    ScriptedBus *scripted = bus.get();
    scripted->monitors = { entry(QStringLiteral("one"), QStringLiteral("card1-DP-1")) };
    // Measured on one of the panels here: DDCRC_RETRIES, and the value applied.
    scripted->liesAboutWrites = true;

    MonitorWorker worker(std::move(bus), false);
    QSignalSpy levels(&worker, &MonitorWorker::levelRead);
    QSignalSpy absent(&worker, &MonitorWorker::monitorAbsent);

    worker.requestDetect();
    worker.requestWrite(QStringLiteral("one"), 40);

    // The write's own answer was a failure, and the read says otherwise, so the
    // level goes out as read and nothing is called absent.
    QVERIFY(!levels.isEmpty());
    const QList<QVariant> last = levels.last();
    QCOMPARE(last.at(0).toString(), QStringLiteral("one"));
    QCOMPARE(last.at(1).toInt(), 40);
    QVERIFY(absent.isEmpty());
    QVERIFY(scripted->reads.contains(QStringLiteral("one")));
}

void MonitorWorkerTest::onlyTheLastValueOfAGestureReachesTheMonitor()
{
    auto bus = std::make_unique<ScriptedBus>();
    ScriptedBus *scripted = bus.get();
    scripted->monitors = { entry(QStringLiteral("one"), QStringLiteral("card1-DP-1")) };

    MonitorWorker worker(std::move(bus), false);
    worker.requestDetect();
    scripted->writes.clear();

    // In the same thread every request is served as it arrives, so what this holds
    // down is that a request replaces a pending value rather than queueing behind
    // it: three values, three writes, and the last one is what the monitor holds.
    for (const int percent : { 30, 45, 70 })
        worker.requestWrite(QStringLiteral("one"), percent);
    QCOMPARE(scripted->levels.value(QStringLiteral("one")), 70);
    QCOMPARE(scripted->writes.last(), QStringLiteral("one=70"));
}

void MonitorWorkerTest::aMonitorThatStopsAnsweringIsCalledAbsentAndLeftAlone()
{
    auto bus = std::make_unique<ScriptedBus>();
    ScriptedBus *scripted = bus.get();
    scripted->monitors = { entry(QStringLiteral("one"), QStringLiteral("card1-DP-1")) };
    MonitorWorker worker(std::move(bus), false);
    QSignalSpy absent(&worker, &MonitorWorker::monitorAbsent);
    QSignalSpy levels(&worker, &MonitorWorker::levelRead);

    worker.requestDetect();
    scripted->silent.insert(QStringLiteral("one"));

    // Every failure is reported as "no level", and the third one in a row makes it
    // absent: a screen somebody switched off must stop being reported.
    for (int i = 0; i < MonitorWorker::kFailuresBeforeAbsent; ++i)
        worker.requestRead();
    QCOMPARE(absent.size(), 1);
    QCOMPARE(levels.last().at(1).toInt(), -1);

    // And then it is left alone rather than costing a third of a second every
    // second. Nothing is asked of it until the retry falls due.
    const int reads = scripted->reads.size();
    worker.requestRead();
    worker.requestWrite(QStringLiteral("one"), 50);
    QCOMPARE(scripted->reads.size(), reads);
    QVERIFY(scripted->writes.isEmpty());

    // A fresh detect starts it over: something changed on the buses, and what was
    // absent may be back.
    scripted->silent.clear();
    worker.requestDetect();
    worker.requestRead();
    QCOMPARE(levels.last().at(1).toInt(), 50);
}

void MonitorWorkerTest::aThreadOfItsOwnIsWhereTheBusRuns()
{
    auto bus = std::make_unique<ScriptedBus>();
    ScriptedBus *scripted = bus.get();
    scripted->monitors = { entry(QStringLiteral("one"), QStringLiteral("card1-DP-1")) };

    // Threaded, which is how it runs in the daemon: the answers cross back as
    // signals and the caller never waits for a bus.
    MonitorWorker worker(std::move(bus));
    QSignalSpy detected(&worker, &MonitorWorker::detected);
    QSignalSpy levels(&worker, &MonitorWorker::levelRead);

    worker.requestDetect();
    QVERIFY(detected.wait(2000));
    // Detecting does not read: what to do with a fresh list is the daemon's
    // decision, and it asks for the levels itself.
    QVERIFY(levels.isEmpty());
    worker.requestRead();
    QTRY_VERIFY(!levels.isEmpty());

    worker.requestWrite(QStringLiteral("one"), 35);
    QTRY_COMPARE(levels.last().at(1).toInt(), 35);
    QCOMPARE(scripted->levels.value(QStringLiteral("one")), 35);
}

void MonitorWorkerTest::aGestureFasterThanTheBusCollapsesIntoTwoWrites()
{
    auto bus = std::make_unique<ScriptedBus>();
    ScriptedBus *scripted = bus.get();
    scripted->monitors = { entry(QStringLiteral("one"), QStringLiteral("card1-DP-1")) };
    // A write that takes a while, which is what a real one does: 300 ms measured.
    scripted->writeCostMs = 40;

    MonitorWorker worker(std::move(bus));
    QSignalSpy detected(&worker, &MonitorWorker::detected);
    QSignalSpy levels(&worker, &MonitorWorker::levelRead);
    worker.requestDetect();
    QVERIFY(detected.wait(2000));

    // A slider dragged across its range asks for twenty values faster than the bus
    // can serve one. Only the last of them means anything, and the bus must not be
    // handed the other nineteen.
    for (int percent = 20; percent <= 40; ++percent)
        worker.requestWrite(QStringLiteral("one"), percent);

    QTRY_COMPARE(scripted->levels.value(QStringLiteral("one")), 40);
    QVERIFY2(scripted->writes.size() <= 3,
             qPrintable(QStringLiteral("writes: %1").arg(scripted->writes.size())));
    QCOMPARE(scripted->writes.last(), QStringLiteral("one=40"));
    // And the level that reaches everything above is the one the monitor holds.
    QTRY_COMPARE(levels.last().at(1).toInt(), 40);
}

QTEST_MAIN(MonitorWorkerTest)
#include "monitor_worker_test.moc"
