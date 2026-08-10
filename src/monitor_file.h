// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "monitor_bus.h"
#include "sysfs.h"

// A monitor bus backed by ordinary files, for every case that is not a machine
// with a monitor plugged into it: the test suite, and the documentation pictures.
//
// It is chosen by the same thing that already means "this is not the real machine"
// — a control surface that is not `/`. There is no production flag for it, because
// a flag that turns the hardware into a fake is a flag somebody eventually leaves
// on.
//
// One file per monitor under `fake-ddc/`, keys on their own lines:
//
//     vendor=DEL
//     model=DELL U3425WE
//     serial=421B3Z3
//     connector=card1-DP-1
//     maximum=100
//     level=85
//
// A write rewrites `level`, which is how a test asserts that a write happened and
// how the screenshot harness makes a monitor exist.
class MonitorFileBus : public MonitorBus {
public:
    explicit MonitorFileBus(Sysfs &sysfs);

    QList<MonitorInfo> detect() override;
    std::optional<int> read(const QString &id) override;
    bool write(const QString &id, int percent) override;

    // The directory the files live in, relative to the control surface's root.
    static QString directory() { return QStringLiteral("fake-ddc"); }

private:
    // Path of the file describing that monitor, or empty when nothing does.
    QString fileFor(const QString &id);

    Sysfs &m_sysfs;
};
