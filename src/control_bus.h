// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "daemon.h"

#include <QObject>
#include <QString>

#include <memory>

// The daemon's control surface: `io.github.svesh87.Sombra` on the system bus.
//
// Everything that drives this daemon arrives here — the console, the command line,
// and anybody with `busctl`. Readings and settings are properties, so a client reads
// them once and is told about every change afterwards; everything that changes state
// is a method, and every method is checked against one polkit action first. The
// person at this screen is answered without a dialog, a remote or inactive session is
// refused, and there is no group to belong to.
//
// This is a second surface beside the three power-profile names in `dbus_service.h`,
// on a connection of its own rather than theirs. Two connections mean two unique
// names and no order of startup: this name is claimed whether or not the
// compatibility names are already held by somebody else, and the other way round.
//
// **Without a bus the daemon still runs.** It owns the thermal envelope, and an
// unreachable bus costs a console rather than the machine's cooling, so the name is
// claimed by a timer that keeps trying. The one refusal to start is another daemon
// already holding the name: two owners of one control surface is what this
// architecture forbids.
class ControlBus : public QObject {
    Q_OBJECT

public:
    struct Options {
        // Empty means the system bus, which is production. A test passes the address
        // of a bus of its own, because claiming this name on the real one would
        // replace the service the machine is running.
        QString busAddress;
        // Appended to the name verbatim, so a test claims
        // `io.github.svesh87.Sombra.t1` and never spells the real name.
        QString nameSuffix;
        // How long between attempts to claim the name. Zero takes it from
        // `SOMBRA_BUS_RETRY_MS`, and failing that the default — a test that waited
        // the production interval twice would spend ten seconds asleep.
        int retryMs = 0;
    };

    ControlBus(Daemon &daemon, Options options, QObject *parent = nullptr);
    ~ControlBus() override;

    // Claims the name and exports the object, and keeps trying if the bus is not
    // there yet. False means another daemon owns this name, which is the one
    // condition worth refusing to start over.
    bool start(QString *error = nullptr);

    // Whether the name is held right now.
    bool owned() const;
    // The name claimed, suffix included.
    QString name() const;

    // Works out what changed and announces it. Called on every tick and after every
    // method, because a console that has just moved a slider should not wait for the
    // next second to see the answer.
    void publish();

private:
    // Exports the object and claims the name, reconnecting first if the bus went
    // away. True once the name is held.
    bool claim();
    // Drops the connection and opens it again. A `dbus-daemon` that was restarted
    // leaves a connection that is dead without saying so, and Qt does not
    // reconnect by itself.
    void reopen();

    class Private;
    std::unique_ptr<Private> d;
};
