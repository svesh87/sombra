// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "monitor_worker.h"

#include <QDateTime>
#include <QMutexLocker>
#include <QMetaObject>

MonitorWorker::MonitorWorker(std::unique_ptr<MonitorBus> bus, bool threaded, QObject *parent)
    : QObject(parent), m_bus(std::move(bus))
{
    if (!threaded)
        return;
    m_thread = new QThread(this);
    // The object moves, so every slot below runs in that thread and every signal
    // out of it crosses back by itself. Nothing else touches the bus.
    moveToThread(m_thread);
    m_thread->start();
}

MonitorWorker::~MonitorWorker()
{
    if (!m_thread)
        return;
    // A read in flight is up to a third of a second, and quitting under it would
    // leave the library mid-exchange on a bus. Waiting is bounded by that.
    m_thread->quit();
    m_thread->wait();
}

void MonitorWorker::requestDetect()
{
    QMetaObject::invokeMethod(this, "doDetect", Qt::AutoConnection);
}

void MonitorWorker::requestRead()
{
    QMetaObject::invokeMethod(this, "doRead", Qt::AutoConnection);
}

void MonitorWorker::requestWrite(const QString &id, int percent)
{
    {
        QMutexLocker locked(&m_lock);
        m_pending.insert(id, percent);
        // One flush in flight. Everything asked for while it works lands in the map
        // and is picked up by the same drain, which is what keeps a slider being
        // dragged from turning into a queue of writes the bus cannot serve.
        if (m_flushQueued)
            return;
        m_flushQueued = true;
    }
    QMetaObject::invokeMethod(this, "doFlush", Qt::AutoConnection);
}

bool MonitorWorker::sleeping(const QString &id) const
{
    const auto until = m_quietUntil.constFind(id);
    if (until == m_quietUntil.constEnd())
        return false;
    return QDateTime::currentSecsSinceEpoch() < *until;
}

void MonitorWorker::noteFailure(const QString &id)
{
    const int failures = m_failures.value(id) + 1;
    m_failures.insert(id, failures);
    if (failures < kFailuresBeforeAbsent)
        return;
    // Absent, and quiet for a while. A monitor switched off would otherwise cost
    // a third of a second every time anything asked, and say so in the log each
    // time as well.
    m_quietUntil.insert(id, QDateTime::currentSecsSinceEpoch() + kRetrySeconds);
    m_failures.insert(id, 0);
    emit monitorAbsent(id);
}

void MonitorWorker::noteSuccess(const QString &id)
{
    m_failures.remove(id);
    m_quietUntil.remove(id);
}

void MonitorWorker::doDetect()
{
    m_known = m_bus->detect();
    m_failures.clear();
    m_quietUntil.clear();
    emit detected(m_known);
}

void MonitorWorker::doRead()
{
    for (const MonitorInfo &monitor : m_known) {
        if (sleeping(monitor.id))
            continue;
        const auto level = m_bus->read(monitor.id);
        if (!level) {
            noteFailure(monitor.id);
            emit levelRead(monitor.id, -1);
            continue;
        }
        noteSuccess(monitor.id);
        emit levelRead(monitor.id, *level);
    }
}

void MonitorWorker::doFlush()
{
    for (;;) {
        QString id;
        int wanted = 0;
        {
            QMutexLocker locked(&m_lock);
            if (m_pending.isEmpty()) {
                m_flushQueued = false;
                return;
            }
            const auto first = m_pending.begin();
            id = first.key();
            wanted = first.value();
            m_pending.erase(first);
        }

        if (sleeping(id))
            continue;

        m_bus->write(id, wanted);
        // The write's own answer is not trusted — a monitor measured here reports a
        // failure and applies the value regardless — so the read decides, and the
        // read is also what everything above learns the new level from.
        const auto level = m_bus->read(id);
        if (!level) {
            noteFailure(id);
            emit levelRead(id, -1);
            continue;
        }
        noteSuccess(id);
        emit levelRead(id, *level);
    }
}
