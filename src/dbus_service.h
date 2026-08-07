// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "daemon.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

// The desktop-facing compatibility surface.
//
// The daemon owns the thermal controls alone, which means it also has to
// answer on the names the desktop already talks to. Three names are claimed:
//
//   net.hadess.PowerProfiles              the standard interface, and the
//   org.freedesktop.UPower.PowerProfiles  primary one
//   com.system76.PowerDaemon              vendor-specific, for the COSMIC
//                                         panel applet
//
// The stock D-Bus policy already permits a root process to own all three, and
// none of them is bus-activatable, so masking the vendor unit hands ownership
// over cleanly and no policy file needs editing.
//
// The graphics half of the vendor interface answers with constants: this
// machine has no discrete GPU, so there is nothing to switch. The daemon never
// touches display brightness on a profile change, which the vendor daemon does
// and which this project treats as a defect.
class DBusService : public QObject {
    Q_OBJECT

public:
    // `busName` selects the bus. Production uses the system bus; tests pass a
    // private bus address so that claiming these names never disturbs the
    // running desktop.
    struct Options {
        // Empty means the system bus.
        QString busAddress;
        // Suffix appended to every claimed name, so a test can claim
        // "net.hadess.PowerProfiles.test1" instead of the real thing.
        QString nameSuffix;
    };

    DBusService(Daemon &daemon, Options options, QObject *parent = nullptr);
    ~DBusService() override;

    // Claims the names and exports the objects. Returns false with a reason
    // when the bus is unreachable or a name is already held; the daemon still
    // runs, because the control socket is the primary interface and the D-Bus
    // names are compatibility.
    bool start(QString *error = nullptr);

    // Names successfully claimed, for the log line and for tests.
    QStringList claimedNames() const;

    // The profile the daemon's active mode currently reads as.
    QString activeProfile() const;

private:
    class Private;
    std::unique_ptr<Private> d;
};
