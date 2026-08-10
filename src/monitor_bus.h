// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>

// Talking to an external monitor's own brightness, which is a different surface
// from the panel's in every way that matters.
//
// The panel is a sysfs node: a write costs microseconds and is believed. A monitor
// is a conversation over an I2C bus at address 0x37 — DDC/CI — and measured on this
// machine it costs about 300 ms to write and 350 ms to read, whichever of three
// monitors was asked. That is why nothing here is called from the daemon's own
// second: the bus lives in a thread of its own, and the loop that owns the
// emergency guard never waits for a screen.
//
// Two more things the measurements settled, and both are in the interface rather
// than in a comment somewhere:
//
//   * a write's return value means nothing. One of the three panels reports
//     DDCRC_RETRIES and applies the value anyway. So `write()` says "the request
//     was sent", and whether it took is decided by reading afterwards.
//   * the internal panel does not answer DDC at all and says so plainly. It stays
//     on sysfs; nothing here is about it.
//
// The interface is substitutable for the same reason `Sysfs` is: the suite runs
// against a bus backed by ordinary files, with no monitor and no permissions.
class MonitorBus;

// What a monitor is, as far as anything above this needs to know.
struct MonitorInfo {
    // Sanitised `<vendor>-<model>-<serial>`, and the connector's name in place of
    // the serial where EDID has none. See monitorId().
    QString id;
    QString vendor;
    QString model;
    // The string serial from EDID, empty when the monitor does not carry one.
    QString serial;
    // The DRM connector, as the kernel names it: "card1-DP-1".
    QString connector;
    // What the monitor says its own scale tops out at. Every monitor measured here
    // said 100, and none of them lied about it, but the number is what percentages
    // are computed against rather than an assumption.
    int maximum = 100;
};

// Everything above this layer speaks in per cent of the monitor's own scale, the
// same units the panel uses, so that one set of settings covers both.
class MonitorBus {
public:
    virtual ~MonitorBus() = default;

    // The monitors that answered. Slow — this is the call that walks every bus —
    // and therefore made when the set of connectors changes rather than on a timer.
    virtual QList<MonitorInfo> detect() = 0;

    // The level now, or nothing when the monitor did not answer. This is also how
    // a write is confirmed, and how a hand on the monitor's own buttons is noticed:
    // measured on this machine, a change made with the monitor's keys reads back.
    virtual std::optional<int> read(const QString &id) = 0;

    // Sends the level. True means the request went out, not that it took: one of
    // the panels measured here reports a failure and applies the value anyway.
    // What decides is the next read.
    virtual bool write(const QString &id, int percent) = 0;
};

// The identifier that settings and telemetry are keyed by.
//
// The string serial from EDID where there is one, because a serial survives being
// unplugged and plugged into the other socket. Where there is none — one of the
// panels measured here has no serial descriptor at all and a placeholder in the
// binary field — the connector's name takes its place. That is worse: the same
// panel in the other socket becomes a monitor the settings have not seen. It is
// still better than two different panels sharing one entry.
//
// Sanitised to `[A-Za-z0-9._-]`, because the identifier travels through a protocol
// that splits on spaces and lands in an INI section name where a slash would be
// read as a separator. EDID has spaces in it: the model of the monitor here is
// "DELL U3425WE".
QString monitorId(const QString &vendor, const QString &model, const QString &serial,
                  const QString &connector);

// The slot this connector's monitor occupies in a sample, or -1 for a connector
// with no slot. The external connectors of the machine, sorted by name, in order:
// on this one that is DP-1, DP-2, HDMI-A-1. The list is what the DRM tree has
// rather than what is plugged in, so a slot means the same socket in every sample
// of a session — and a connector a DP-MST hub invents at run time gets no slot,
// because a slot that moved would make a session unreadable.
int monitorSlot(const QString &connector, const QStringList &externalConnectors);

// One byte that says which monitor is in a slot: not an identifier, a change of
// hardware. Never zero, because zero means the slot is empty.
uint8_t monitorFingerprint(const QString &id);
