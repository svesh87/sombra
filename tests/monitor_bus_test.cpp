// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "fake_sysfs.h"
#include "monitor_bus.h"
#include "monitor_file.h"

#include <QTest>

// The identity of a monitor, the slot it occupies, and the bus that answers from
// files. The real bus is not here: it needs a monitor, a bus and root, and what it
// does with what the library says is measured by hand rather than asserted.
class MonitorBusTest : public QObject {
    Q_OBJECT

private slots:
    void theIdentifierSurvivesEverythingEdidPutsInIt();
    void aMonitorWithoutASerialIsKeyedByItsSocket();
    void slotsAreSocketsAndNotArrivalOrder();
    void theFingerprintIsNeverZeroAndChangesWithTheMonitor();
    void theFileBusReadsWritesAndForgetsNothingElse();
};

void MonitorBusTest::theIdentifierSurvivesEverythingEdidPutsInIt()
{
    // The model of the monitor measured here has a space in it, and the identifier
    // travels through a protocol that splits on spaces and into an INI section name
    // where a slash is a separator.
    const QString id = monitorId(QStringLiteral("DEL"), QStringLiteral("DELL U3425WE"),
                                 QStringLiteral("421B3Z3"), QStringLiteral("card1-DP-1"));
    QCOMPARE(id, QStringLiteral("DEL-DELL_U3425WE-421B3Z3"));
    QVERIFY(!id.contains(QLatin1Char(' ')));
    QVERIFY(!id.contains(QLatin1Char('/')));

    // Runs of rubbish collapse instead of becoming a row of underscores, and the
    // ends stay clean.
    QCOMPARE(monitorId(QStringLiteral(" ACME "), QStringLiteral("A//B  C"),
                       QStringLiteral("s/n 1"), QStringLiteral("card1-DP-2")),
             QStringLiteral("ACME-A_B_C-s_n_1"));

    // A monitor that said nothing readable is still addressable: the socket is the
    // one thing always known about it.
    QCOMPARE(monitorId(QString(), QString(), QString(), QStringLiteral("card1-HDMI-A-1")),
             QStringLiteral("card1-HDMI-A-1"));
}

void MonitorBusTest::aMonitorWithoutASerialIsKeyedByItsSocket()
{
    // One of the panels measured here has no serial descriptor at all. The socket
    // takes its place: worse, because the same panel in the other socket becomes a
    // monitor the settings have not seen, and better than two different panels
    // sharing one entry.
    const QString id = monitorId(QStringLiteral("WCS"), QStringLiteral("AQ14T"), QString(),
                                 QStringLiteral("card1-DP-2"));
    QCOMPARE(id, QStringLiteral("WCS-AQ14T-card1-DP-2"));
    QVERIFY(id != monitorId(QStringLiteral("WCS"), QStringLiteral("AQ14T"), QString(),
                            QStringLiteral("card1-DP-1")));

    // Whitespace is not a serial.
    QCOMPARE(monitorId(QStringLiteral("WCS"), QStringLiteral("AQ14T"), QStringLiteral("   "),
                       QStringLiteral("card1-DP-2")),
             id);
}

void MonitorBusTest::slotsAreSocketsAndNotArrivalOrder()
{
    // The external connectors of this machine, sorted, are the slots of a sample.
    const QStringList external = { QStringLiteral("card1-DP-1"), QStringLiteral("card1-DP-2"),
                                   QStringLiteral("card1-HDMI-A-1") };
    QCOMPARE(monitorSlot(QStringLiteral("card1-DP-1"), external), 0);
    QCOMPARE(monitorSlot(QStringLiteral("card1-DP-2"), external), 1);
    QCOMPARE(monitorSlot(QStringLiteral("card1-HDMI-A-1"), external), 2);

    // A connector a DP-MST hub invents at run time gets no slot: a slot that moved
    // between samples would make a session unreadable.
    const QStringList withHub = external + QStringList { QStringLiteral("card1-DP-9") };
    QCOMPARE(monitorSlot(QStringLiteral("card1-DP-9"), withHub), -1);
    QCOMPARE(monitorSlot(QStringLiteral("card1-eDP-1"), external), -1);
    QCOMPARE(monitorSlot(QString(), external), -1);
}

void MonitorBusTest::theFingerprintIsNeverZeroAndChangesWithTheMonitor()
{
    // Zero means "the slot is empty", so no monitor may hash to it.
    QCOMPARE(monitorFingerprint(QString()), uint8_t(0));
    QVERIFY(monitorFingerprint(QStringLiteral("DEL-DELL_U3425WE-421B3Z3")) != 0);
    QVERIFY(monitorFingerprint(QStringLiteral("WCS-AQ14T-card1-DP-2")) != 0);
    // Three panels went through one socket in an hour on this desk. Telling them
    // apart afterwards is the whole job of this byte.
    QVERIFY(monitorFingerprint(QStringLiteral("RTK-TYPEC-L56051794302"))
            != monitorFingerprint(QStringLiteral("WCS-AQ14T-card1-DP-2")));
    // And it is stable: the same monitor is the same byte in every sample.
    QCOMPARE(monitorFingerprint(QStringLiteral("DEL-DELL_U3425WE-421B3Z3")),
             monitorFingerprint(QStringLiteral("DEL-DELL_U3425WE-421B3Z3")));
}

void MonitorBusTest::theFileBusReadsWritesAndForgetsNothingElse()
{
    FakeSysfs tree;
    Sysfs sysfs(tree.root());
    MonitorFileBus bus(sysfs);

    // Nothing plugged in is the tree's default, and an empty bus is not an error.
    QVERIFY(bus.detect().isEmpty());
    QVERIFY(!bus.read(QStringLiteral("DEL-DELL_U3425WE-421B3Z3")));
    QVERIFY(!bus.write(QStringLiteral("DEL-DELL_U3425WE-421B3Z3"), 50));

    tree.plugMonitor(QStringLiteral("DP-1"), QStringLiteral("DEL"),
                     QStringLiteral("DELL U3425WE"), QStringLiteral("421B3Z3"), 85);
    tree.plugMonitor(QStringLiteral("DP-2"), QStringLiteral("WCS"), QStringLiteral("AQ14T"),
                     QString(), 20);

    const QList<MonitorInfo> found = bus.detect();
    QCOMPARE(found.size(), 2);
    QCOMPARE(found.at(0).id, QStringLiteral("DEL-DELL_U3425WE-421B3Z3"));
    QCOMPARE(found.at(0).model, QStringLiteral("DELL U3425WE"));
    QCOMPARE(found.at(0).connector, QStringLiteral("card1-DP-1"));
    QCOMPARE(found.at(1).id, QStringLiteral("WCS-AQ14T-card1-DP-2"));

    QCOMPARE(bus.read(found.at(0).id).value_or(-1), 85);
    QCOMPARE(bus.read(found.at(1).id).value_or(-1), 20);

    QVERIFY(bus.write(found.at(1).id, 65));
    QCOMPARE(bus.read(found.at(1).id).value_or(-1), 65);
    // A write touches one monitor and rewrites nothing else about it: the identity
    // is still readable afterwards, which is what a rewritten file could lose.
    QCOMPARE(bus.read(found.at(0).id).value_or(-1), 85);
    QCOMPARE(bus.detect().at(1).id, found.at(1).id);

    // Out of range is clamped rather than written through: a monitor's scale ends
    // where it ends.
    QVERIFY(bus.write(found.at(0).id, 400));
    QCOMPARE(bus.read(found.at(0).id).value_or(-1), 100);
}

QTEST_MAIN(MonitorBusTest)
#include "monitor_bus_test.moc"
