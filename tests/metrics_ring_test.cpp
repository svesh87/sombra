// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "metrics_ring.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

namespace {

Sample sampleAt(uint32_t ts, uint8_t pl1 = 20)
{
    Sample s {};
    s.ts = ts;
    s.pkgTemp = 600;
    s.pl1W = pl1;
    return s;
}

} // namespace

class MetricsRingTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void geometryIsFixedAndFileSizeFollowsFromIt();
    void evictsByWrappingRatherThanGrowing();
    void survivesReopen();
    void toleratesTruncatedTail();
    void readsATruncatedRingWithoutRepairingIt();
    void reinitialisesOnVersionMismatch();
    void reinitialisesOnCapacityMismatch();
    void markNewestStampsTheLastRecord();
    void readOnlyMappingRefusesWrites();
    void refusesZeroCapacity();
    void closedRingIsInert();

private:
    QTemporaryDir m_dir;
    QString m_path;
};

void MetricsRingTest::init()
{
    m_path = m_dir.path() + QStringLiteral("/metrics.ring");
    QFile::remove(m_path);
}

void MetricsRingTest::geometryIsFixedAndFileSizeFollowsFromIt()
{
    QCOMPARE(sizeof(Sample), size_t(64));
    QCOMPARE(sizeof(RingHeader), size_t(64));

    MetricsRing ring;
    QVERIFY(ring.open(m_path, 100));
    QCOMPARE(QFile(m_path).size(), qint64(sizeof(RingHeader) + 100 * sizeof(Sample)));
    QCOMPARE(ring.capacity(), uint64_t(100));
    QCOMPARE(ring.count(), uint64_t(0));

    // Seven days at 1 Hz is what the shipped capacity has to hold.
    QCOMPARE(kRingCapacity, uint64_t(7 * 24 * 60 * 60));
}

void MetricsRingTest::evictsByWrappingRatherThanGrowing()
{
    MetricsRing ring;
    QVERIFY(ring.open(m_path, 8));
    const qint64 sizeWhenEmpty = QFile(m_path).size();

    for (uint32_t i = 1; i <= 20; ++i)
        ring.append(sampleAt(1000 + i));

    QCOMPARE(QFile(m_path).size(), sizeWhenEmpty);
    QCOMPARE(ring.count(), uint64_t(8));
    QCOMPARE(ring.sequence(), uint64_t(20));
    // The oldest surviving record is the thirteenth, not the first.
    QCOMPARE(ring.at(0).ts, uint32_t(1013));
    QCOMPARE(ring.at(7).ts, uint32_t(1020));
    QCOMPARE(ring.at(8).ts, uint32_t(0));
}

void MetricsRingTest::survivesReopen()
{
    {
        MetricsRing ring;
        QVERIFY(ring.open(m_path, 16));
        for (uint32_t i = 1; i <= 5; ++i)
            ring.append(sampleAt(2000 + i, static_cast<uint8_t>(i)));
        ring.flush();
    }

    MetricsRing reopened;
    QVERIFY(reopened.open(m_path, 16));
    QCOMPARE(reopened.count(), uint64_t(5));
    QCOMPARE(reopened.at(0).ts, uint32_t(2001));
    QCOMPARE(reopened.at(4).pl1W, uint8_t(5));

    reopened.append(sampleAt(3000));
    QCOMPARE(reopened.count(), uint64_t(6));
    QCOMPARE(reopened.at(5).ts, uint32_t(3000));
}

void MetricsRingTest::toleratesTruncatedTail()
{
    {
        MetricsRing ring;
        QVERIFY(ring.open(m_path, 16));
        for (uint32_t i = 1; i <= 10; ++i)
            ring.append(sampleAt(4000 + i));
    }

    // Lose the second half of the body, as an interrupted write would.
    QFile file(m_path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.resize(static_cast<qint64>(sizeof(RingHeader) + 6 * sizeof(Sample))));
    file.close();

    MetricsRing ring;
    QVERIFY(ring.open(m_path, 16));
    QCOMPARE(QFile(m_path).size(), qint64(sizeof(RingHeader) + 16 * sizeof(Sample)));
    QCOMPARE(ring.count(), uint64_t(10));
    // What survived is still readable; what did not reads as never-written.
    QCOMPARE(ring.at(0).ts, uint32_t(4001));
    QCOMPARE(ring.at(5).ts, uint32_t(4006));
    QCOMPARE(ring.at(6).ts, uint32_t(0));

    ring.append(sampleAt(5000));
    QCOMPARE(ring.at(10).ts, uint32_t(5000));
}

void MetricsRingTest::readsATruncatedRingWithoutRepairingIt()
{
    {
        MetricsRing ring;
        QVERIFY(ring.open(m_path, 16));
        for (uint32_t i = 1; i <= 10; ++i)
            ring.append(sampleAt(4100 + i));
    }

    QFile file(m_path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.resize(static_cast<qint64>(sizeof(RingHeader) + 6 * sizeof(Sample))));
    file.close();

    // The export path must not resize the store it is reading.
    MetricsRing reader;
    QString error;
    QVERIFY2(reader.openExisting(m_path, &error), qPrintable(error));
    QCOMPARE(QFile(m_path).size(), qint64(sizeof(RingHeader) + 6 * sizeof(Sample)));
    QCOMPARE(reader.count(), uint64_t(10));
    QCOMPARE(reader.at(0).ts, uint32_t(4101));
    QCOMPARE(reader.at(5).ts, uint32_t(4106));
    QCOMPARE(reader.at(6).ts, uint32_t(0));
    QCOMPARE(reader.at(9).ts, uint32_t(0));
}

void MetricsRingTest::reinitialisesOnVersionMismatch()
{
    {
        MetricsRing ring;
        QVERIFY(ring.open(m_path, 16));
        ring.append(sampleAt(6000));
    }

    QFile file(m_path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    RingHeader header {};
    QCOMPARE(file.read(reinterpret_cast<char *>(&header), sizeof(header)), qint64(sizeof(header)));
    header.version = kRingVersion + 1;
    QVERIFY(file.seek(0));
    QCOMPARE(file.write(reinterpret_cast<const char *>(&header), sizeof(header)),
             qint64(sizeof(header)));
    file.close();

    MetricsRing ring;
    QVERIFY(ring.open(m_path, 16));
    QCOMPARE(ring.count(), uint64_t(0));
    QCOMPARE(ring.sequence(), uint64_t(0));
}

void MetricsRingTest::reinitialisesOnCapacityMismatch()
{
    {
        MetricsRing ring;
        QVERIFY(ring.open(m_path, 16));
        ring.append(sampleAt(7000));
        QCOMPARE(ring.count(), uint64_t(1));
    }

    MetricsRing resized;
    QVERIFY(resized.open(m_path, 32));
    QCOMPARE(resized.capacity(), uint64_t(32));
    QCOMPARE(resized.count(), uint64_t(0));
    QCOMPARE(QFile(m_path).size(), qint64(sizeof(RingHeader) + 32 * sizeof(Sample)));
}

void MetricsRingTest::markNewestStampsTheLastRecord()
{
    MetricsRing ring;
    QVERIFY(ring.open(m_path, 4));
    QVERIFY(!ring.markNewest(Mark::Good));

    ring.append(sampleAt(8001));
    ring.append(sampleAt(8002));
    QVERIFY(ring.markNewest(Mark::TooLoud));
    QCOMPARE(ring.at(1).mark, uint8_t(Mark::TooLoud));
    QCOMPARE(ring.at(0).mark, uint8_t(Mark::None));
}

void MetricsRingTest::readOnlyMappingRefusesWrites()
{
    {
        MetricsRing ring;
        QVERIFY(ring.open(m_path, 8));
        ring.append(sampleAt(9001));
        ring.append(sampleAt(9002));
    }

    MetricsRing reader;
    QString error;
    QVERIFY2(reader.openExisting(m_path, &error), qPrintable(error));
    QVERIFY(reader.isReadOnly());
    QCOMPARE(reader.capacity(), uint64_t(8));
    QCOMPARE(reader.count(), uint64_t(2));

    reader.append(sampleAt(9003));
    reader.flush();
    QVERIFY(!reader.markNewest(Mark::Good));
    QCOMPARE(reader.count(), uint64_t(2));

    // A reader must never be able to reinitialise the store.
    MetricsRing again;
    QVERIFY(again.open(m_path, 8));
    QCOMPARE(again.count(), uint64_t(2));
}

void MetricsRingTest::refusesZeroCapacity()
{
    MetricsRing ring;
    QString error;
    QVERIFY(!ring.open(m_path, 0, &error));
    QVERIFY(!error.isEmpty());

    QVERIFY(!ring.openExisting(m_dir.path() + QStringLiteral("/absent.ring"), &error));

    // A file that is not a ring at all is refused rather than adopted.
    const QString stray = m_dir.path() + QStringLiteral("/stray.bin");
    QFile file(stray);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(200, 'x'));
    file.close();
    QVERIFY(!ring.openExisting(stray, &error));
}

void MetricsRingTest::closedRingIsInert()
{
    MetricsRing ring;
    QVERIFY(!ring.isOpen());
    QCOMPARE(ring.capacity(), uint64_t(0));
    QCOMPARE(ring.count(), uint64_t(0));
    QCOMPARE(ring.sequence(), uint64_t(0));
    QCOMPARE(ring.at(0).ts, uint32_t(0));
    ring.append(sampleAt(1));
    ring.flush();
    QVERIFY(!ring.markNewest(Mark::Good));
}

QTEST_MAIN(MetricsRingTest)
#include "metrics_ring_test.moc"
