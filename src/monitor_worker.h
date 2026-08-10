// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "monitor_bus.h"

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QThread>

#include <memory>

// The monitors, kept away from the daemon's own second.
//
// Measured on this machine: a read costs about 350 ms and a write about 300 ms, per
// monitor, and two monitors are two buses in sequence. The 1 Hz loop is where the
// emergency guard lives, so it does not wait for a screen — the bus is worked in a
// thread of its own and the answers arrive as signals.
//
// What the queue does, and why it is a queue at all:
//
//   * one pending write per monitor, and a new one replaces it. A slider being
//     dragged asks for twenty values and only the last one means anything;
//   * a read is scheduled per monitor and coalesces the same way;
//   * a monitor that stops answering — switched off, or its input moved to another
//     machine — is retried at a slow cadence rather than every second, and is
//     called absent after a few failures in a row.
//
// The worker owns the bus and is the only thread that touches it: `libddcutil`
// promises nothing about being called from two.
class MonitorWorker : public QObject {
    Q_OBJECT

public:
    // How many failures in a row make a monitor absent, and how long before a
    // monitor that is absent is tried again. Both are about a screen somebody
    // switched off: it must stop being reported without the log filling up.
    static constexpr int kFailuresBeforeAbsent = 3;
    static constexpr int kRetrySeconds = 30;

    // Takes the bus. `threaded` false keeps everything in the calling thread,
    // which is what the tests use: a queue is easier to reason about when the
    // answers arrive before the next line.
    explicit MonitorWorker(std::unique_ptr<MonitorBus> bus, bool threaded = true,
                           QObject *parent = nullptr);
    ~MonitorWorker() override;

    // Asks for a fresh list. Cheap to call and slow to answer: this is the call
    // that walks every bus.
    void requestDetect();
    // Asks for the level of every monitor known to answer.
    void requestRead();
    // Asks for a level. The last one asked for wins; a request while a write is in
    // flight replaces the value rather than queueing behind it.
    void requestWrite(const QString &id, int percent);

signals:
    // The list changed: what answered, in the order the bus reported.
    void detected(const QList<MonitorInfo> &monitors);
    // A level was read. `percent` below zero means the monitor did not answer.
    void levelRead(const QString &id, int percent);
    // Three failures in a row: nothing more will be asked of it for a while.
    void monitorAbsent(const QString &id);

private slots:
    void doDetect();
    void doRead();
    // Drains the pending values, newest per monitor, until there are none left. One
    // of these is queued at a time: a gesture that asks for thirty values while one
    // write is on the bus leaves one value behind it, not twenty-nine.
    void doFlush();

private:
    // Nothing is asked of a monitor that has just failed, until the retry is due.
    bool sleeping(const QString &id) const;
    void noteFailure(const QString &id);
    void noteSuccess(const QString &id);

    std::unique_ptr<MonitorBus> m_bus;
    QThread *m_thread = nullptr;
    QList<MonitorInfo> m_known;
    QHash<QString, int> m_failures;
    QHash<QString, qint64> m_quietUntil;  // monotonic seconds
    // The write that has not gone out yet, per monitor. A value here replaces an
    // older one; there is no history of what a slider passed through on its way.
    // Touched from the thread that asks and from the one that writes, so it has a
    // lock of its own — the only shared state there is.
    mutable QMutex m_lock;
    QHash<QString, int> m_pending;
    bool m_flushQueued = false;
};
