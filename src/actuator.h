// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "setpoints.h"
#include "sysfs.h"

#include <QHash>
#include <QString>
#include <QStringList>

// The only writer of the thermal control surface.
//
// Two processes writing these nodes produce race-dependent behaviour rather
// than redundancy, which is why this daemon masks the vendor one instead of
// layering on it. The actuator therefore remembers what it last wrote to each
// node: a value that disagrees with that memory was put there by something
// else, which is the signal that ownership has been taken back.
//
// Writes are idempotent. A node is read first and written only when it
// disagrees, so the steady state costs reads and no writes.
class Actuator {
public:
    explicit Actuator(Sysfs &sysfs);

    // Locates every node. Called at startup and again whenever a node that was
    // resolved has since disappeared, which happens when a hwmon device
    // re-registers within a boot.
    void resolve();

    // Applies in the order the firmware requires and verifies by reading back.
    // Disagreements are recorded as flags rather than papered over with a
    // second write in the same tick.
    void apply(const Setpoints &wanted);

    // Restores automatic fan control and a confirmed-safe power state. Called
    // on every exit path the daemon controls. A fan left behind by a dead
    // daemon is a defect, and nothing else will pick up after this process.
    void restore();

    // True when at least one control node is present. False means the tree is
    // not a machine this daemon can manage at all.
    bool hasAnyControl() const;

    // Any disagreement seen during the last apply: a foreign value, a node
    // that disappeared, or a write that did not take.
    bool lastApplyMismatched() const;
    // A node that resolve() found and that has since disappeared. The daemon
    // re-resolves on this rather than writing into a stale path for the rest
    // of the boot.
    bool nodesVanished() const { return m_vanished; }

    const QStringList &missingNodes() const { return m_missing; }

private:
    void writeIfDifferent(const QString &path, const QString &value);
    void writeIfDifferent(const QString &path, qint64 value);

    Sysfs &m_sysfs;
    bool m_foreign = false;
    bool m_vanished = false;
    bool m_writeFailed = false;
    QStringList m_missing;
    // Last value this daemon put in each node, which is what makes a foreign
    // write distinguishable from a setpoint change of its own.
    QHash<QString, QString> m_written;

    QString m_platformProfilePath;
    QString m_raplPackage;
    QString m_pwmPath;
    QString m_gtMaxPath;
    QStringList m_eppPaths;
};
