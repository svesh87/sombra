// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "ring_migrate.h"

#include "metrics_ring.h"

#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace {

// The released layouts, frozen. These are copies of what `struct Sample` was at
// the time, and they are never edited again — a change here would silently
// change how an old file is read.

#pragma pack(push, 1)
// Version 1. No backlight window: the automation had not been written yet.
struct SampleV1 {
    uint32_t ts;
    int16_t pkgTemp;
    int16_t boardTemp;
    int16_t nvmeTemp;
    uint16_t fanRpm;
    uint16_t pkgPower;
    uint16_t corePower;
    uint16_t uncorePower;
    uint16_t cpuMhz;
    uint16_t gpuMhz;
    uint16_t load1;
    uint16_t pkgThrottle;
    uint16_t coreThrottle;

    uint8_t modeId;
    uint8_t pl1W;
    uint8_t pl2W;
    uint8_t maxPerfPct;
    uint8_t noTurbo;
    uint8_t epp;
    uint8_t hwpBoost;
    uint8_t profile;
    uint16_t gtMaxMhz;
    uint8_t fanMode;
    uint8_t chargeLimit;
    uint8_t flags;
    uint8_t mark;

    uint16_t light;
    uint8_t brightness;
    uint8_t brightnessBase;
    uint8_t brightnessProfile;

    uint8_t reserved[17];
};

// Version 2. The backlight window as it was then: a span either side of the
// base, bounded by a floor and a ceiling, all in percentage points.
struct SampleV2 {
    uint32_t ts;
    int16_t pkgTemp;
    int16_t boardTemp;
    int16_t nvmeTemp;
    uint16_t fanRpm;
    uint16_t pkgPower;
    uint16_t corePower;
    uint16_t uncorePower;
    uint16_t cpuMhz;
    uint16_t gpuMhz;
    uint16_t load1;
    uint16_t pkgThrottle;
    uint16_t coreThrottle;

    uint8_t modeId;
    uint8_t pl1W;
    uint8_t pl2W;
    uint8_t maxPerfPct;
    uint8_t noTurbo;
    uint8_t epp;
    uint8_t hwpBoost;
    uint8_t profile;
    uint16_t gtMaxMhz;
    uint8_t fanMode;
    uint8_t chargeLimit;
    uint8_t flags;
    uint8_t mark;

    uint16_t light;
    uint8_t brightness;
    uint8_t brightnessBase;
    uint8_t brightnessProfile;
    uint8_t brightnessSpan;
    uint8_t brightnessFloor;
    uint8_t brightnessCeiling;

    uint8_t reserved[14];
};
#pragma pack(pop)

static_assert(sizeof(SampleV1) == 64, "a released record was 64 bytes and stays that way");
static_assert(sizeof(SampleV2) == 64, "a released record was 64 bytes and stays that way");

SampleV2 upgrade(const SampleV1 &in)
{
    SampleV2 out {};
    // The prefix through brightnessProfile is byte-identical: version 2 only
    // appended into the reserved bytes.
    std::memcpy(&out, &in, offsetof(SampleV1, reserved));
    // The window these samples were taken under is not recorded anywhere, and
    // guessing it would put a number in the data that nobody measured.
    out.brightnessSpan = kAbsentPercent;
    out.brightnessFloor = kAbsentPercent;
    out.brightnessCeiling = kAbsentPercent;
    return out;
}

Sample upgrade(const SampleV2 &in)
{
    Sample out {};
    out.ts = in.ts;
    out.pkgTemp = in.pkgTemp;
    out.boardTemp = in.boardTemp;
    out.nvmeTemp = in.nvmeTemp;
    out.fanRpm = in.fanRpm;
    out.pkgPower = in.pkgPower;
    out.corePower = in.corePower;
    out.uncorePower = in.uncorePower;
    out.cpuMhz = in.cpuMhz;
    out.gpuMhz = in.gpuMhz;
    out.load1 = in.load1;
    out.pkgThrottle = in.pkgThrottle;
    out.coreThrottle = in.coreThrottle;
    // Nobody counted the displays at the time, and that is a fact about the
    // sample rather than a gap to fill in.
    out.displays = kAbsentPercent;
    out.modeId = in.modeId;
    out.pl1W = in.pl1W;
    out.pl2W = in.pl2W;
    out.maxPerfPct = in.maxPerfPct;
    out.noTurbo = in.noTurbo;
    out.epp = in.epp;
    out.hwpBoost = in.hwpBoost;
    out.profile = in.profile;
    out.gtMaxMhz = in.gtMaxMhz;
    out.fanMode = in.fanMode;
    out.chargeLimit = in.chargeLimit;
    out.flags = in.flags;
    out.mark = in.mark;

    // The old field held the sensor's raw count, and this sensor counts in
    // thousandths of a lux, so the number carries across unchanged. What does
    // not carry across is the range: sixteen bits stopped at 65.534 lux, and
    // every brighter room in an old file reads as that same value. Those
    // samples are a floor on the light, not a measurement of it.
    out.light = in.light == kAbsentValue ? kAbsentLight : in.light;
    out.brightness = in.brightness;
    out.brightnessBase = in.brightnessBase;
    // The span was measured either side of the base, which is what the
    // response means now. The ceiling is gone: it is 100 for everybody.
    out.brightnessResponse = in.brightnessSpan;
    out.brightnessFloor = in.brightnessFloor;
    return out;
}

// The chain, applied to one record. Each version enters at its own step and
// falls through the rest.
Sample toCurrent(uint16_t version, const char *record)
{
    if (version == kRingVersion) {
        Sample current {};
        std::memcpy(&current, record, sizeof(current));
        return current;
    }
    if (version == 1) {
        SampleV1 v1 {};
        std::memcpy(&v1, record, sizeof(v1));
        return upgrade(upgrade(v1));
    }
    SampleV2 v2 {};
    std::memcpy(&v2, record, sizeof(v2));
    return upgrade(v2);
}

} // namespace

bool canMigrateRing(uint16_t version)
{
    return version == 1 || version == 2;
}

QList<Sample> migratedSamples(const QString &path, QString *note)
{
    QList<Sample> carried;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return carried;

    RingHeader head {};
    if (file.read(reinterpret_cast<char *>(&head), sizeof(head)) != sizeof(head))
        return carried;
    if (head.magic != kRingMagic || head.recordSize != sizeof(Sample) || head.capacity == 0)
        return carried;
    if (head.version != kRingVersion && !canMigrateRing(head.version))
        return carried;

    // A file shorter than its header claims lost its tail to a truncated
    // write. The slots that are gone are simply not read.
    const uint64_t inFile =
            (static_cast<uint64_t>(file.size()) - sizeof(RingHeader)) / sizeof(Sample);
    // `slots` would be the Qt keyword.
    const uint64_t present = std::min<uint64_t>(head.capacity, inFile);
    if (present == 0)
        return carried;

    const uint64_t count = std::min<uint64_t>(head.seq, present);
    const uint64_t oldest = head.seq < head.capacity ? 0 : head.seq - head.capacity;
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t slot = (oldest + i) % head.capacity;
        if (slot >= present)
            continue;
        if (!file.seek(sizeof(RingHeader) + slot * sizeof(Sample)))
            continue;
        char record[sizeof(Sample)] = {};
        if (file.read(record, sizeof(record)) != sizeof(record))
            continue;
        const Sample sample = toCurrent(head.version, record);
        // A slot that was never written has no timestamp, and carrying it over
        // would invent a sample dated 1970.
        if (sample.ts != 0)
            carried.append(sample);
    }

    if (note && !carried.isEmpty()) {
        *note = head.version == kRingVersion
                ? QStringLiteral("%1: kept %2 samples across the change of size")
                          .arg(path)
                          .arg(carried.size())
                : QStringLiteral("%1: carried %2 samples over from format version %3")
                          .arg(path)
                          .arg(carried.size())
                          .arg(head.version);
    }
    return carried;
}

QString setRingAside(const QString &path)
{
    // One slot, replaced every time. A version in the name would make each
    // rollback leave its own file behind — down from 4 to 3 and then to 2 gives
    // three rings on a disk whose endurance the whole storage design is built
    // around, and nobody reads the middle one. The layout is in the header, so
    // the name does not have to carry it.
    const QString kept = path + QStringLiteral(".bak");
    if (QFileInfo::exists(kept) && !QFile::remove(kept))
        return {};
    if (!QFile::rename(path, kept))
        return {};
    return kept;
}
