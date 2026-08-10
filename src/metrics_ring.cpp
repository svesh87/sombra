// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "metrics_ring.h"

#include "ring_migrate.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

QString describeErrno(const QString &what)
{
    return what + QStringLiteral(": ") + QString::fromLocal8Bit(std::strerror(errno));
}

} // namespace

MetricsRing::MetricsRing() = default;

MetricsRing::~MetricsRing()
{
    close();
}

RingHeader *MetricsRing::header() const
{
    return static_cast<RingHeader *>(m_map);
}

Sample *MetricsRing::records() const
{
    return reinterpret_cast<Sample *>(static_cast<char *>(m_map) + sizeof(RingHeader));
}

namespace {

// Clamps and says so, on the same terms as every other numeric setting: a value
// out of range is a mistake worth reporting rather than a reason to refuse the
// whole edit.
int clampReported(int value, int low, int high, const QString &what, QStringList *clamped)
{
    if (value >= low && value <= high)
        return value;
    const int bounded = std::clamp(value, low, high);
    if (clamped) {
        clamped->append(QStringLiteral("%1: %2 is outside %3..%4, using %5")
                                .arg(what)
                                .arg(value)
                                .arg(low)
                                .arg(high)
                                .arg(bounded));
    }
    return bounded;
}

} // namespace

uint64_t retentionCapacity(int days)
{
    return static_cast<uint64_t>(std::clamp(days, kRetentionMinDays, kRetentionMaxDays))
            * kSamplesPerDay;
}

MetricsSettings builtinMetricsSettings()
{
    return MetricsSettings {};
}

bool applyMetricsSetting(MetricsSettings &settings, const QString &key, const QString &value,
                         QString *error, QStringList *clamped)
{
    const QString what = QStringLiteral("metrics: %1").arg(key);
    const QString text = value.trimmed();

    if (key == QLatin1String("enabled")) {
        // The same words the state file takes for a switch, because a person
        // editing one file should not have to learn a second vocabulary.
        const QString lowered = text.toLower();
        if (lowered == QLatin1String("on") || lowered == QLatin1String("true")
            || lowered == QLatin1String("1") || lowered == QLatin1String("yes")) {
            settings.enabled = true;
            return true;
        }
        if (lowered == QLatin1String("off") || lowered == QLatin1String("false")
            || lowered == QLatin1String("0") || lowered == QLatin1String("no")) {
            settings.enabled = false;
            return true;
        }
        if (error)
            *error = QStringLiteral("%1: '%2' is not on or off").arg(what, value);
        return false;
    }

    if (key == QLatin1String("retention_days")) {
        bool ok = false;
        const int number = text.toInt(&ok);
        if (!ok) {
            if (error)
                *error = QStringLiteral("%1: '%2' is not a number").arg(what, value);
            return false;
        }
        settings.retentionDays =
                clampReported(number, kRetentionMinDays, kRetentionMaxDays, what, clamped);
        return true;
    }

    if (error)
        *error = QStringLiteral("metrics: unknown setting '%1'").arg(key);
    return false;
}

bool loadMetricsSettings(const QString &path, MetricsSettings *settings, QStringList *problems)
{
    if (!QFileInfo::exists(path))
        return false;
    QSettings file(path, QSettings::IniFormat);
    if (file.status() != QSettings::NoError)
        return false;
    if (!file.childGroups().contains(QStringLiteral("metrics")))
        return false;

    MetricsSettings parsed = builtinMetricsSettings();
    file.beginGroup(QStringLiteral("metrics"));
    for (const QString &key : file.childKeys()) {
        QString error;
        const QString value = file.value(key).toString();
        if (!applyMetricsSetting(parsed, key.toLower(), value, &error, problems) && problems)
            problems->append(error + QStringLiteral(", ignored"));
    }
    file.endGroup();

    *settings = parsed;
    return true;
}

QString metricsSettingsSection(const MetricsSettings &settings)
{
    QString text;
    text += QStringLiteral("[metrics]\n");
    text += QStringLiteral("retention_days=%1\n").arg(settings.retentionDays);
    text += QStringLiteral("enabled=%1\n").arg(settings.enabled ? QStringLiteral("on")
                                                                 : QStringLiteral("off"));
    return text;
}

bool MetricsRing::open(const QString &path, uint64_t capacity, QString *error)
{
    close();

    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    if (capacity == 0)
        return fail(QStringLiteral("ring capacity must be non-zero"));
    // The geometry is operator input. Without this the size computation below
    // wraps and the mapping ends up smaller than the header claims, which
    // turns every later append into an out-of-bounds write.
    if (capacity > (SIZE_MAX - sizeof(RingHeader)) / sizeof(Sample))
        return fail(QStringLiteral("ring capacity %1 does not fit in memory").arg(capacity));

    // Settled before the file is touched: reading the old records needs the old
    // bytes, and setting a file aside renames the very file the call below
    // would otherwise create.
    QList<Sample> carried;
    m_note.clear();
    if (!prepareExisting(path, capacity, &carried))
        return fail(m_note);

    QDir().mkpath(QFileInfo(path).absolutePath());

    const int fd = ::open(path.toLocal8Bit().constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return fail(describeErrno(QStringLiteral("open %1").arg(path)));

    const size_t wanted = sizeof(RingHeader) + static_cast<size_t>(capacity) * sizeof(Sample);

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const QString message = describeErrno(QStringLiteral("fstat %1").arg(path));
        ::close(fd);
        return fail(message);
    }

    // A short file is extended rather than rejected. The tail may have been
    // lost to a truncated write; the records it held read back as
    // never-written, which readers already have to handle for a fresh ring.
    if (static_cast<size_t>(st.st_size) != wanted) {
        if (::ftruncate(fd, static_cast<off_t>(wanted)) != 0) {
            const QString message = describeErrno(QStringLiteral("ftruncate %1").arg(path));
            ::close(fd);
            return fail(message);
        }
    }
    // Blocks are reserved here rather than left to be allocated page by page
    // as the ring fills. A sparse mapping would turn a full filesystem into a
    // SIGBUS days later, in the middle of a write, where the daemon cannot
    // restore the hardware; reserving up front turns the same condition into
    // an error message at startup.
    if (const int code = ::posix_fallocate(fd, 0, static_cast<off_t>(wanted))) {
        errno = code;
        const QString message = describeErrno(QStringLiteral("allocate %1").arg(path));
        ::close(fd);
        return fail(message);
    }

    void *map = ::mmap(nullptr, wanted, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED)
        return fail(describeErrno(QStringLiteral("mmap %1").arg(path)));

    m_map = map;
    m_mapSize = wanted;
    m_slots = capacity;
    m_path = path;
    m_readOnly = false;

    RingHeader *head = header();
    const bool geometryMatches = head->magic == kRingMagic && head->version == kRingVersion
            && head->recordSize == sizeof(Sample) && head->capacity == capacity;
    if (!geometryMatches) {
        // A file that already existed carries someone else's records; a file
        // just created by this call is already zero, and touching every page
        // of it would dirty the whole ring for nothing.
        if (st.st_size > 0)
            std::memset(m_map, 0, m_mapSize);
        head->magic = kRingMagic;
        head->version = kRingVersion;
        head->recordSize = sizeof(Sample);
        head->capacity = capacity;
        head->seq = 0;
    }

    // Converted records go in oldest first, so the sequence number ends up
    // where it would have been had they always been in this layout. More of
    // them than the ring holds means the oldest are dropped, which is what the
    // ring does with them anyway.
    if (!carried.isEmpty()) {
        const qsizetype first =
                std::max<qsizetype>(0, carried.size() - static_cast<qsizetype>(capacity));
        for (qsizetype i = first; i < carried.size(); ++i)
            append(carried.at(i));
    }

    return true;
}

bool MetricsRing::prepareExisting(const QString &path, uint64_t capacity, QList<Sample> *carried)
{
    QFile file(path);
    if (!file.exists() || file.size() == 0)
        return true;

    RingHeader head {};
    const bool readable = file.open(QIODevice::ReadOnly)
            && file.read(reinterpret_cast<char *>(&head), sizeof(head)) == sizeof(head);
    file.close();

    // The record size is checked against the version in the header, not against
    // the one this build compiles: those two are no longer the same number for
    // every version, and confusing them would set aside a ring that migrates
    // perfectly well.
    const uint16_t stride = ringRecordSize(head.version);
    const bool isRing = readable && head.magic == kRingMagic && stride != 0
            && head.recordSize == stride;
    // Nothing to do for a ring this build wrote at this size: it is mapped as
    // it stands, and reading it out only to write it back would be work for
    // the sake of it.
    if (isRing && head.version == kRingVersion && head.capacity == capacity)
        return true;

    // A different capacity is the same operation as a different layout — the
    // file is rebuilt either way, and the records may as well come along.
    if (isRing && (head.version == kRingVersion || canMigrateRing(head.version))) {
        *carried = migratedSamples(path, &m_note);
        return true;
    }

    // Everything else keeps its bytes. A version from a newer build after a
    // rollback, a layout that was never released, and a file whose header is
    // damaged are the same case here: this build cannot read it, deleting what
    // it cannot read is not its decision to make, and a ring it cannot read is
    // no reason to stop managing the machine.
    const QString kept = setRingAside(path);
    if (kept.isEmpty()) {
        m_note = QStringLiteral("%1 is not a ring this build can read, and it could not be "
                                "moved out of the way")
                         .arg(path);
        return false;
    }
    m_note = QStringLiteral("%1 was not a ring this build can read; kept as %2, starting a "
                            "fresh one")
                     .arg(path, kept);
    return true;
}

bool MetricsRing::openExisting(const QString &path, QString *error)
{
    close();

    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    const int fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return fail(describeErrno(QStringLiteral("open %1").arg(path)));

    struct stat st {};
    if (::fstat(fd, &st) != 0 || static_cast<size_t>(st.st_size) < sizeof(RingHeader)) {
        ::close(fd);
        return fail(QStringLiteral("%1 is not a metrics ring: too short").arg(path));
    }

    void *map = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED)
        return fail(describeErrno(QStringLiteral("mmap %1").arg(path)));

    const RingHeader *head = static_cast<const RingHeader *>(map);
    const bool usable = head->magic == kRingMagic && head->version == kRingVersion
            && head->recordSize == sizeof(Sample) && head->capacity > 0;
    if (!usable) {
        ::munmap(map, static_cast<size_t>(st.st_size));
        return fail(QStringLiteral("%1 is not a metrics ring of version %2")
                            .arg(path)
                            .arg(kRingVersion));
    }

    m_map = map;
    m_mapSize = static_cast<size_t>(st.st_size);
    // A file shorter than its header claims has lost its tail. A reader does
    // not repair it — the daemon does that when it next opens the ring — so
    // the records that are gone simply read as never-written.
    m_slots = std::min<uint64_t>(head->capacity,
                                 (m_mapSize - sizeof(RingHeader)) / sizeof(Sample));
    m_path = path;
    m_readOnly = true;
    return true;
}

void MetricsRing::close()
{
    if (m_map) {
        if (!m_readOnly)
            ::msync(m_map, m_mapSize, MS_SYNC);
        ::munmap(m_map, m_mapSize);
    }
    m_map = nullptr;
    m_mapSize = 0;
    m_slots = 0;
    m_readOnly = false;
    m_path.clear();
}

void MetricsRing::append(const Sample &sample)
{
    if (!isOpen() || m_readOnly)
        return;
    RingHeader *head = header();
    const uint64_t slot = head->seq % head->capacity;
    records()[slot] = sample;
    // Publish the record before the sequence number that makes it visible.
    std::atomic_thread_fence(std::memory_order_release);
    head->seq = head->seq + 1;
}

bool MetricsRing::markNewest(Mark mark)
{
    if (!isOpen() || m_readOnly)
        return false;
    RingHeader *head = header();
    if (head->seq == 0)
        return false;
    const uint64_t slot = (head->seq - 1) % head->capacity;
    records()[slot].mark = static_cast<uint8_t>(mark);
    return true;
}

void MetricsRing::flush()
{
    if (!isOpen() || m_readOnly)
        return;
    // Asynchronous on purpose: the point of the mapping is that the daemon
    // never waits on the disk.
    ::msync(m_map, m_mapSize, MS_ASYNC);
}

uint64_t MetricsRing::capacity() const
{
    return isOpen() ? header()->capacity : 0;
}

uint64_t MetricsRing::sequence() const
{
    return isOpen() ? header()->seq : 0;
}

uint64_t MetricsRing::count() const
{
    if (!isOpen())
        return 0;
    const RingHeader *head = header();
    return head->seq < head->capacity ? head->seq : head->capacity;
}

Sample MetricsRing::at(uint64_t index) const
{
    Sample empty {};
    if (!isOpen() || index >= count())
        return empty;
    const RingHeader *head = header();
    const uint64_t oldest = head->seq < head->capacity ? 0 : head->seq - head->capacity;
    const uint64_t slot = (oldest + index) % head->capacity;
    if (slot >= m_slots)
        return empty;
    return records()[slot];
}
