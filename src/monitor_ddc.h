// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "monitor_bus.h"
#include "sysfs.h"

#include <QHash>

// The real bus: DDC/CI over `libddcutil`, VCP feature 0x10.
//
// Two decisions in here come straight out of the measurements on this machine, and
// neither is obvious from the library's documentation.
//
// The library verifies a write by reading it back immediately, and that is turned
// off. One of the three monitors measured here is not ready to answer that soon: it
// reports DDCRC_RETRIES after 1344 ms, while the value it was given is sitting in
// the monitor. With verification off the same write takes 302 ms and reports
// success. Confirming is left to the next ordinary read, which happens anyway.
//
// A handle is opened per operation rather than held. A handle kept between calls is
// a handle held across somebody unplugging the monitor, and the cost measured is in
// the exchange rather than in the open.
//
// The library is called from one thread only — its thread safety is not promised —
// which is what MonitorWorker is for.
class MonitorDdcBus : public MonitorBus {
public:
    explicit MonitorDdcBus(Sysfs &sysfs);

    QList<MonitorInfo> detect() override;
    std::optional<int> read(const QString &id) override;
    bool write(const QString &id, int percent) override;

private:
    // The DRM connector on that I2C bus, from `/sys/class/drm/*/ddc`. The library
    // of this version reports the bus number and not the connector, and the
    // connector is what a person recognises and what a slot in a sample means.
    QString connectorForBus(int busno);

    Sysfs &m_sysfs;
    // What the last detect() found: the opaque reference the library needs, by the
    // identifier everything above uses. Rebuilt on every detect(), because a
    // reference outlives neither a replug nor a rescan.
    QHash<QString, void *> m_references;
    QHash<QString, int> m_maximum;
};
