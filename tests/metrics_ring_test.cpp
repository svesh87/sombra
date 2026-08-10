// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "metrics_ring.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <cstring>

namespace {

Sample sampleAt(uint32_t ts, uint8_t pl1 = 20)
{
    Sample s {};
    s.ts = ts;
    s.pkgTemp = 600;
    s.pl1W = pl1;
    return s;
}

QByteArray contentsOf(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void setHeaderVersion(const QString &path, uint16_t version)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    RingHeader header {};
    QCOMPARE(file.read(reinterpret_cast<char *>(&header), sizeof(header)), qint64(sizeof(header)));
    header.version = version;
    QVERIFY(file.seek(0));
    QCOMPARE(file.write(reinterpret_cast<const char *>(&header), sizeof(header)),
             qint64(sizeof(header)));
}

// A ring in a released layout, byte for byte, built here rather than by the
// code under test: a migration test that generates its input with the same
// definitions it is checking would pass whatever those definitions said.
//
// The two layouts share a prefix and differ in what follows the backlight
// profile index, so the records are assembled from offsets.
void writeLegacyRing(const QString &path, uint16_t version)
{
    constexpr int kRecord = 64;
    const int capacity = 8;

    QByteArray file(sizeof(RingHeader) + capacity * kRecord, '\0');
    RingHeader header {};
    header.magic = kRingMagic;
    header.version = version;
    header.recordSize = kRecord;
    header.capacity = capacity;
    header.seq = 2;
    std::memcpy(file.data(), &header, sizeof(header));

    for (int i = 0; i < 2; ++i) {
        char record[kRecord] = {};
        const auto put = [&record](int offset, const auto &value) {
            std::memcpy(record + offset, &value, sizeof(value));
        };
        put(0, uint32_t(1000 + i));       // ts
        put(4, int16_t(812));             // pkgTemp
        put(10, uint16_t(2400));          // fanRpm
        put(28, uint8_t(0));              // modeId
        put(29, uint8_t(20));             // pl1W
        // The light reading, and the backlight level beside it. The second
        // sample has no reading at all, which is a different thing from dark.
        put(42, uint16_t(i == 0 ? 25900 : kAbsentValue));
        put(44, uint8_t(70));             // brightness
        put(45, uint8_t(73));             // brightnessBase
        put(46, uint8_t(0));              // brightnessProfile
        if (version == 2) {
            put(47, uint8_t(30));         // brightnessSpan
            put(48, uint8_t(5));          // brightnessFloor
            put(49, uint8_t(100));        // brightnessCeiling
        }
        std::memcpy(file.data() + sizeof(RingHeader) + i * kRecord, record, kRecord);
    }

    QFile out(path);
    QVERIFY(out.open(QIODevice::WriteOnly));
    QCOMPARE(out.write(file), qint64(file.size()));
}

// The last 64-byte layout, version 4, written the same way and for the same
// reason: by offsets, so the test does not agree with the code by construction.
//
// This one matters more than the others. The record used to be 64 bytes for every
// released version, so a check written as "recordSize == sizeof(Sample)" passed by
// accident; with a 128-byte record it fails for every older ring and would send a
// week of samples to the set-aside slot.
void writeVersionFourRing(const QString &path)
{
    constexpr int kRecord = 64;
    const int capacity = 8;

    QByteArray file(sizeof(RingHeader) + capacity * kRecord, '\0');
    RingHeader header {};
    header.magic = kRingMagic;
    header.version = 4;
    header.recordSize = kRecord;
    header.capacity = capacity;
    header.seq = 2;
    std::memcpy(file.data(), &header, sizeof(header));

    for (int i = 0; i < 2; ++i) {
        char record[kRecord] = {};
        const auto put = [&record](int offset, const auto &value) {
            std::memcpy(record + offset, &value, sizeof(value));
        };
        put(0, uint32_t(2000 + i));  // ts
        put(4, int16_t(795));        // pkgTemp
        put(10, uint16_t(3100));     // fanRpm
        put(28, uint8_t(3));         // displays
        put(29, uint8_t(1));         // modeId
        put(30, uint8_t(20));        // pl1W
        put(43, uint32_t(19400));    // light, millilux in four bytes by then
        put(47, uint8_t(65));        // brightness
        put(48, uint8_t(68));        // brightnessBase
        put(49, uint8_t(30));        // brightnessResponse
        put(50, uint8_t(5));         // brightnessFloor
        std::memcpy(file.data() + sizeof(RingHeader) + i * kRecord, record, kRecord);
    }

    QFile out(path);
    QVERIFY(out.open(QIODevice::WriteOnly));
    QCOMPARE(out.write(file), qint64(file.size()));
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
    void keepsARingItCannotReadInsteadOfDeletingIt();
    void aDamagedRingCostsTheSamplesAndNothingElse();
    void keepsTheRecordsAcrossAChangeOfCapacity();
    void carriesVersionOneSamplesForward();
    void carriesVersionTwoSamplesForward();
    void carriesVersionFourSamplesForward();
    void aRecordSizeThatDisagreesWithItsVersionIsNotARing();
    void markNewestStampsTheLastRecord();
    void readOnlyMappingRefusesWrites();
    void refusesZeroCapacity();
    void closedRingIsInert();
    void retentionIsDaysOfSamplesWithinItsBounds();
    void theMetricsSectionReadsBackAsTheSameSettings();

private:
    QTemporaryDir m_dir;
    QString m_path;
};

void MetricsRingTest::init()
{
    m_path = m_dir.path() + QStringLiteral("/metrics.ring");
    QFile::remove(m_path);
    // The set-aside slot too: cases that assert nothing was set aside are
    // otherwise answered by the leftovers of the case before them.
    QFile::remove(m_path + QStringLiteral(".bak"));
}

void MetricsRingTest::geometryIsFixedAndFileSizeFollowsFromIt()
{
    QCOMPARE(sizeof(Sample), size_t(128));
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

void MetricsRingTest::keepsARingItCannotReadInsteadOfDeletingIt()
{
    // What a rollback looks like from inside the older build: the file on disk
    // announces a layout from a version that did not exist when this one was
    // written.
    {
        MetricsRing ring;
        QVERIFY(ring.open(m_path, 16));
        ring.append(sampleAt(6000));
    }
    setHeaderVersion(m_path, kRingVersion + 1);
    const QByteArray before = contentsOf(m_path);

    MetricsRing ring;
    QVERIFY(ring.open(m_path, 16));
    // A fresh ring in its place, and the daemon keeps recording.
    QCOMPARE(ring.count(), uint64_t(0));
    QCOMPARE(ring.sequence(), uint64_t(0));
    ring.append(sampleAt(6001));
    QCOMPARE(ring.count(), uint64_t(1));
    ring.close();

    const QString kept = m_path + QStringLiteral(".bak");
    QVERIFY2(QFile::exists(kept), qPrintable(ring.note()));
    QCOMPARE(contentsOf(kept), before);
    QVERIFY(ring.note().contains(kept));

    // A second rollback replaces that copy instead of leaving a second one
    // beside it. Down from one version to the next and then to the next again
    // would otherwise strew the directory with rings nobody reads.
    setHeaderVersion(m_path, kRingVersion + 1);
    const QByteArray afterTheSecond = contentsOf(m_path);
    MetricsRing third;
    QVERIFY(third.open(m_path, 16));
    QCOMPARE(third.count(), uint64_t(0));

    const QDir directory(QFileInfo(m_path).absolutePath());
    QCOMPARE(directory.entryList(QDir::Files).size(), 2);
    QCOMPARE(contentsOf(kept), afterTheSecond);
}

void MetricsRingTest::aDamagedRingCostsTheSamplesAndNothingElse()
{
    {
        MetricsRing ring;
        QVERIFY(ring.open(m_path, 16));
        ring.append(sampleAt(8000));
    }
    // A header nothing wrote deliberately: the machine lost power mid-write, or
    // the file is somebody else's entirely.
    QFile file(m_path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QCOMPARE(file.write(QByteArray(sizeof(RingHeader), '\x5a')), qint64(sizeof(RingHeader)));
    file.close();

    MetricsRing ring;
    QVERIFY(ring.open(m_path, 16));
    QVERIFY(QFile::exists(m_path + QStringLiteral(".bak")));
    ring.append(sampleAt(8001));
    QCOMPARE(ring.count(), uint64_t(1));
    QCOMPARE(ring.at(0).ts, uint32_t(8001));
}

void MetricsRingTest::keepsTheRecordsAcrossAChangeOfCapacity()
{
    {
        MetricsRing ring;
        QVERIFY(ring.open(m_path, 16));
        for (uint32_t ts = 7000; ts < 7005; ++ts)
            ring.append(sampleAt(ts));
    }

    MetricsRing grown;
    QVERIFY(grown.open(m_path, 32));
    QCOMPARE(grown.capacity(), uint64_t(32));
    QCOMPARE(grown.count(), uint64_t(5));
    QCOMPARE(grown.at(0).ts, uint32_t(7000));
    QCOMPARE(grown.at(4).ts, uint32_t(7004));
    QCOMPARE(QFile(m_path).size(), qint64(sizeof(RingHeader) + 32 * sizeof(Sample)));
    grown.close();

    // Shrinking keeps the newest, which is the same rule the ring applies when
    // it wraps.
    MetricsRing shrunk;
    QVERIFY(shrunk.open(m_path, 3));
    QCOMPARE(shrunk.count(), uint64_t(3));
    QCOMPARE(shrunk.at(0).ts, uint32_t(7002));
    QCOMPARE(shrunk.at(2).ts, uint32_t(7004));
}

void MetricsRingTest::carriesVersionOneSamplesForward()
{
    writeLegacyRing(m_path, 1);

    MetricsRing ring;
    QVERIFY(ring.open(m_path, 16));
    QCOMPARE(ring.count(), uint64_t(2));
    QVERIFY2(ring.note().contains(QStringLiteral("version 1")), qPrintable(ring.note()));

    const Sample first = ring.at(0);
    QCOMPARE(first.ts, uint32_t(1000));
    QCOMPARE(first.pkgTemp, int16_t(812));
    QCOMPARE(first.pl1W, uint8_t(20));
    QCOMPARE(first.brightness, uint8_t(70));
    QCOMPARE(first.light, uint32_t(25900));
    // Version 1 had no window and nobody counted displays: absent, not zero.
    QCOMPARE(first.brightnessResponse, kAbsentPercent);
    QCOMPARE(first.brightnessFloor, kAbsentPercent);
    QCOMPARE(first.displays, kAbsentPercent);
    QCOMPARE(ring.at(1).ts, uint32_t(1001));
}

void MetricsRingTest::carriesVersionTwoSamplesForward()
{
    writeLegacyRing(m_path, 2);

    MetricsRing ring;
    QVERIFY(ring.open(m_path, 16));
    QCOMPARE(ring.count(), uint64_t(2));
    QVERIFY2(ring.note().contains(QStringLiteral("version 2")), qPrintable(ring.note()));

    const Sample first = ring.at(0);
    QCOMPARE(first.ts, uint32_t(1000));
    QCOMPARE(first.fanRpm, uint16_t(2400));
    // The raw count was thousandths of a lux already, so the number stands.
    QCOMPARE(first.light, uint32_t(25900));
    // The span either side of the base is what the response means now.
    QCOMPARE(first.brightnessResponse, uint8_t(30));
    QCOMPARE(first.brightnessFloor, uint8_t(5));
    QCOMPARE(first.displays, kAbsentPercent);

    // An absent reading stays absent rather than becoming the largest lux
    // value in the file.
    QCOMPARE(ring.at(1).light, kAbsentLight);
}

void MetricsRingTest::retentionIsDaysOfSamplesWithinItsBounds()
{
    QCOMPARE(retentionCapacity(kRetentionDefaultDays), kRingCapacity);
    QCOMPARE(retentionCapacity(1), kSamplesPerDay);
    QCOMPARE(retentionCapacity(30), 30 * kSamplesPerDay);
    // Out of range is clamped rather than believed: a ring of zero records has no
    // meaning and one of a thousand days is not what a spin box asked for.
    QCOMPARE(retentionCapacity(0), kSamplesPerDay * kRetentionMinDays);
    QCOMPARE(retentionCapacity(-5), kSamplesPerDay * kRetentionMinDays);
    QCOMPARE(retentionCapacity(1000), kSamplesPerDay * kRetentionMaxDays);

    MetricsSettings settings = builtinMetricsSettings();
    QCOMPARE(settings.retentionDays, kRetentionDefaultDays);
    QVERIFY(settings.enabled);

    QString error;
    QStringList clamped;
    QVERIFY(applyMetricsSetting(settings, QStringLiteral("retention_days"),
                                QStringLiteral("30"), &error, &clamped));
    QCOMPARE(settings.retentionDays, 30);
    QVERIFY(clamped.isEmpty());

    // Clamped and said out loud, so nobody spends a week expecting samples that
    // were never going to be kept.
    QVERIFY(applyMetricsSetting(settings, QStringLiteral("retention_days"),
                                QStringLiteral("90"), &error, &clamped));
    QCOMPARE(settings.retentionDays, kRetentionMaxDays);
    QVERIFY2(clamped.join(QLatin1Char(';')).contains(QStringLiteral("outside")),
             qPrintable(clamped.join(QLatin1Char(';'))));

    for (const char *word : { "off", "false", "0", "no" }) {
        QVERIFY(applyMetricsSetting(settings, QStringLiteral("enabled"),
                                    QString::fromLatin1(word), &error, &clamped));
        QVERIFY(!settings.enabled);
    }
    for (const char *word : { "on", "true", "1", "yes" }) {
        QVERIFY(applyMetricsSetting(settings, QStringLiteral("enabled"),
                                    QString::fromLatin1(word), &error, &clamped));
        QVERIFY(settings.enabled);
    }

    // Nonsense is refused rather than guessed at, the same as everywhere else.
    QVERIFY(!applyMetricsSetting(settings, QStringLiteral("enabled"),
                                 QStringLiteral("sometimes"), &error, &clamped));
    QVERIFY(!error.isEmpty());
    QVERIFY(!applyMetricsSetting(settings, QStringLiteral("retention_days"),
                                 QStringLiteral("a fortnight"), &error, &clamped));
    QVERIFY(!applyMetricsSetting(settings, QStringLiteral("depth"), QStringLiteral("7"),
                                 &error, &clamped));
}

void MetricsRingTest::theMetricsSectionReadsBackAsTheSameSettings()
{
    MetricsSettings written;
    written.retentionDays = 21;
    written.enabled = false;

    const QString path = m_dir.path() + QStringLiteral("/settings.conf");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(metricsSettingsSection(written).toUtf8()),
             qint64(metricsSettingsSection(written).toUtf8().size()));
    file.close();

    MetricsSettings read = builtinMetricsSettings();
    QStringList problems;
    QVERIFY(loadMetricsSettings(path, &read, &problems));
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
    QCOMPARE(read.retentionDays, 21);
    QVERIFY(!read.enabled);

    // A file with no section of ours leaves the argument alone, which is how a
    // fresh install falls back to the built-in values.
    MetricsSettings untouched = builtinMetricsSettings();
    const QString empty = m_dir.path() + QStringLiteral("/empty.conf");
    QFile other(empty);
    QVERIFY(other.open(QIODevice::WriteOnly));
    other.write("[general]\ndefault_mode=work\n");
    other.close();
    QVERIFY(!loadMetricsSettings(empty, &untouched, &problems));
    QCOMPARE(untouched.retentionDays, kRetentionDefaultDays);
}

void MetricsRingTest::carriesVersionFourSamplesForward()
{
    writeVersionFourRing(m_path);

    MetricsRing ring;
    QVERIFY(ring.open(m_path, 16));
    QCOMPARE(ring.count(), uint64_t(2));
    QVERIFY2(ring.note().contains(QStringLiteral("version 4")), qPrintable(ring.note()));
    // Carried, not set aside. The whole point: the samples are still here.
    QVERIFY(!QFileInfo::exists(m_path + QStringLiteral(".bak")));

    const Sample first = ring.at(0);
    QCOMPARE(first.ts, uint32_t(2000));
    QCOMPARE(first.fanRpm, uint16_t(3100));
    QCOMPARE(first.displays, uint8_t(3));
    QCOMPARE(first.light, uint32_t(19400));
    QCOMPARE(first.brightness, uint8_t(65));
    QCOMPARE(first.brightnessBase, uint8_t(68));
    QCOMPARE(first.brightnessResponse, uint8_t(30));
    QCOMPARE(first.brightnessFloor, uint8_t(5));

    // What version 4 did not have is absent rather than zero: zero is a legal
    // ceiling and a legal brightness, and nobody wrote these down at the time.
    QCOMPARE(first.brightnessCeiling, kAbsentPercent);
    QCOMPARE(first.monitors, kAbsentPercent);
    for (const MonitorSlot &slot : first.monitorSlots) {
        QCOMPARE(slot.level, kAbsentPercent);
        QCOMPARE(slot.base, kAbsentPercent);
        QCOMPARE(slot.floor, kAbsentPercent);
        QCOMPARE(slot.ceiling, kAbsentPercent);
        QCOMPARE(slot.response, kAbsentPercent);
        QCOMPARE(slot.id, uint8_t(0));
    }
    QCOMPARE(first.monitorFlags, uint8_t(0));
}

void MetricsRingTest::aRecordSizeThatDisagreesWithItsVersionIsNotARing()
{
    // A file claiming the current version with a 64-byte record is not something
    // this build wrote, and reading it as if it were would misread every field.
    // It keeps its bytes, the same as any other unreadable ring.
    writeVersionFourRing(m_path);
    setHeaderVersion(m_path, kRingVersion);

    MetricsRing ring;
    QVERIFY(ring.open(m_path, 16));
    QCOMPARE(ring.count(), uint64_t(0));
    QVERIFY(QFileInfo::exists(m_path + QStringLiteral(".bak")));
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
