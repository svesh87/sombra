// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "sample.h"

#include <QList>
#include <QString>

#include <cstdint>

// Seven days at 1 Hz. Retention is a property of the geometry: the file's
// blocks are reserved once at this size and the oldest record is overwritten
// in place, so there is no growth and no background cleanup task to fail.
inline constexpr uint64_t kRingCapacity = 604800;

inline constexpr uint32_t kRingMagic = 0x52424d4f;  // "OMBR" little-endian
// Raised whenever the meaning of the record changes. An existing ring in an
// older released layout is converted rather than discarded — see
// ring_migrate.h — and one this build cannot read is moved aside with its
// bytes intact.
//
// Version 3 is not in the chain and never will be: it existed in a working
// tree and was never released, so a file claiming it is not a known layout.
inline constexpr uint16_t kRingVersion = 4;

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

    // Creates the file if absent. An existing file in an older released layout
    // has its samples converted into this one; a file this build cannot read is
    // moved aside with its bytes intact, and a fresh ring takes its place.
    // Either way note() then says what happened.
    //
    // A file shorter than its header claims is extended, and the records that
    // were lost read back as never-written. A change of capacity keeps the
    // newest records that still fit.
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

    // What open() did with an existing file, when that is worth saying out
    // loud: samples carried over, or a file set aside. Empty otherwise.
    const QString &note() const { return m_note; }

private:
    RingHeader *header() const;
    Sample *records() const;
    // Converts or sets aside whatever is already at the path, and returns the
    // samples to carry into the new ring. False means the file is in the way
    // and could not be moved, which is the one case where opening fails rather
    // than starting fresh.
    bool prepareExisting(const QString &path, uint64_t capacity, QList<Sample> *carried);

    QString m_path;
    QString m_note;
    void *m_map = nullptr;
    size_t m_mapSize = 0;
    // Record slots physically present in the mapping. Equal to the capacity
    // for a writable ring, which is extended on open; smaller for a read-only
    // mapping of a file whose tail was lost.
    uint64_t m_slots = 0;
    bool m_readOnly = false;
};
