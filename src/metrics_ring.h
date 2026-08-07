// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "sample.h"

#include <QString>

#include <cstdint>

// Seven days at 1 Hz. Retention is a property of the geometry: the file's
// blocks are reserved once at this size and the oldest record is overwritten
// in place, so there is no growth and no background cleanup task to fail.
inline constexpr uint64_t kRingCapacity = 604800;

inline constexpr uint32_t kRingMagic = 0x52424d4f;  // "OMBR" little-endian
// Raised whenever the meaning of the record changes. There is no migration
// path: an existing ring with a different version is reinitialised, because a
// telemetry ring has no history worth converting.
inline constexpr uint16_t kRingVersion = 1;

#pragma pack(push, 1)
struct RingHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t recordSize;
    uint64_t capacity;  // records
    uint64_t seq;       // records ever appended; the newest sits at (seq-1) % capacity
    uint8_t reserved[40];
};
#pragma pack(pop)

static_assert(sizeof(RingHeader) == 64, "the header occupies exactly one record slot");

// A fixed-size ring of samples held in a shared mapping.
//
// The mapping is the store: appending writes into the page cache and costs no
// I/O. A sample dirties one page and consecutive samples share it, so the
// kernel's writeback carries roughly one page per interval to the disk instead
// of one write per sample. flush() asks for that writeback to start; it does
// not hold it back, and it is not what makes the rate low — the record size
// is.
//
// Crash behaviour follows from the write order. A record is written first and
// the sequence number published afterwards, so a process that dies between the
// two loses the newest sample rather than exposing a half-written one. A
// reader running concurrently with the daemon can still catch the oldest slot
// mid-overwrite and emit one row that mixes two samples; at one wrap per
// retention period that is not worth a sequence lock in a file whose purpose
// is trend analysis.
class MetricsRing {
public:
    MetricsRing();
    ~MetricsRing();

    MetricsRing(const MetricsRing &) = delete;
    MetricsRing &operator=(const MetricsRing &) = delete;

    // Creates the file if absent. An existing file whose header does not match
    // this build's geometry is reinitialised: the format carries no migration
    // path, because a telemetry ring has no history worth migrating.
    // A file shorter than its header claims is extended, and the records that
    // were lost read back as never-written.
    bool open(const QString &path, uint64_t capacity, QString *error = nullptr);

    // Maps an existing ring read-only, adopting whatever geometry its header
    // declares. The export tool uses this: a reader must never be able to
    // reinitialise the store by guessing the capacity wrong.
    bool openExisting(const QString &path, QString *error = nullptr);

    void close();
    bool isOpen() const { return m_map != nullptr; }
    bool isReadOnly() const { return m_readOnly; }

    void append(const Sample &sample);

    // Stamps the newest record. Used for the subjective marks, which are made
    // after the sample they refer to has already been written.
    bool markNewest(Mark mark);

    void flush();

    uint64_t capacity() const;
    // Records the sequence number says exist, oldest first. Slots that were
    // never written — a ring that has not filled yet, or one whose tail a
    // truncation removed — are included and carry ts == 0; readers skip them.
    uint64_t count() const;
    Sample at(uint64_t index) const;

    // Total appended over the file's lifetime, including evicted records.
    uint64_t sequence() const;

    const QString &path() const { return m_path; }

private:
    RingHeader *header() const;
    Sample *records() const;

    QString m_path;
    void *m_map = nullptr;
    size_t m_mapSize = 0;
    // Record slots physically present in the mapping. Equal to the capacity
    // for a writable ring, which is extended on open; smaller for a read-only
    // mapping of a file whose tail was lost.
    uint64_t m_slots = 0;
    bool m_readOnly = false;
};
