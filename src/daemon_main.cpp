// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "control_bus.h"
#include "daemon.h"
#include "dbus_service.h"
#include "signal_pipe.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>

#include <csignal>
#include <memory>

#include <cstdio>
#include <unistd.h>

namespace {

// Leaves without running anything else's teardown, and that is deliberate.
//
// The monitor library starts a thread at load time to watch the display
// connectors over udev. A process that exits soon after loading it — `--version`,
// `--help`, or a refusal to start — terminates that thread while it is still
// starting, and the library segfaults doing it: measured here, four times out of
// five on `--version`. Nothing of ours is left unfinished at this point. The
// daemon's own shutdown is explicit and has already run where there was one: the
// fan is back on automatic, the ring is flushed, the state is written. Only other
// people's atexit handlers are skipped, and one of them is known to crash.
[[noreturn]] void leave(int code)
{
    std::fflush(nullptr);
    ::_exit(code);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("sombrad"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
            QStringLiteral("Thermal and power daemon for the ASUS Zenbook 14 UX3405MA."));
    // Kept for their text, not for their behaviour: Qt answers both of these by
    // calling ::exit() itself, which is the one exit this process must not take —
    // see leave() above.
    const QCommandLineOption helpOption = parser.addHelpOption();
    const QCommandLineOption versionOption = parser.addVersionOption();

    DaemonOptions defaults;
    const QCommandLineOption rootOption(
            QStringLiteral("root"),
            QStringLiteral("Root of the kernel control surface. Anything other than / points the "
                           "daemon at a fake tree and is meant for testing."),
            QStringLiteral("dir"), defaults.sysfsRoot);
    const QCommandLineOption configOption(QStringLiteral("config"),
                                          QStringLiteral("Mode definitions."),
                                          QStringLiteral("file"), defaults.configPath);
    const QCommandLineOption stateOption(QStringLiteral("state"),
                                         QStringLiteral("File holding the selected mode."),
                                         QStringLiteral("file"), defaults.statePath);
    const QCommandLineOption ringOption(QStringLiteral("ring"),
                                        QStringLiteral("Telemetry ring."), QStringLiteral("file"),
                                        defaults.ringPath);
    const QCommandLineOption busOption(
            QStringLiteral("bus-address"),
            QStringLiteral("Bus to answer on. Empty means the system bus, which is the only "
                           "one a service uses; an address here points the daemon at a bus of "
                           "its own and is meant for testing."),
            QStringLiteral("address"));
    const QCommandLineOption capacityOption(
            QStringLiteral("capacity"),
            QStringLiteral("Ring capacity in records, overriding the retention in the "
                           "settings file; one record per second."),
            QStringLiteral("n"), QStringLiteral("0"));
    const QCommandLineOption ticksOption(
            QStringLiteral("ticks"),
            QStringLiteral("Exit after this many ticks instead of running until signalled."),
            QStringLiteral("n"), QStringLiteral("0"));
    const QCommandLineOption intervalOption(QStringLiteral("interval-ms"),
                                            QStringLiteral("Sampling interval."),
                                            QStringLiteral("ms"),
                                            QString::number(defaults.intervalMs));
    const QCommandLineOption noDbusOption(
            QStringLiteral("no-dbus"),
            QStringLiteral("Do not claim the desktop power-profile names. Those are "
                           "compatibility for the panel applet; the daemon's own interface is "
                           "claimed either way."));
    parser.addOptions({ rootOption, configOption, stateOption, ringOption, busOption,
                        capacityOption, ticksOption, intervalOption, noDbusOption });
    if (!parser.parse(QCoreApplication::arguments())) {
        qWarning().noquote() << parser.errorText();
        leave(1);
    }
    if (parser.isSet(versionOption)) {
        std::printf("%s %s\n", qPrintable(QCoreApplication::applicationName()),
                    qPrintable(QCoreApplication::applicationVersion()));
        leave(0);
    }
    if (parser.isSet(helpOption)) {
        std::fputs(qPrintable(parser.helpText()), stdout);
        leave(0);
    }

    DaemonOptions options;
    options.sysfsRoot = parser.value(rootOption);
    options.configPath = parser.value(configOption);
    options.statePath = parser.value(stateOption);
    options.ringPath = parser.value(ringOption);
    options.ringCapacity = parser.value(capacityOption).toULongLong();
    options.maxTicks = parser.value(ticksOption).toInt();
    options.intervalMs = parser.value(intervalOption).toInt();
    if (options.intervalMs <= 0)
        options.intervalMs = defaults.intervalMs;

    Daemon daemon(options);

    // The fan must return to automatic control on every exit path this process
    // controls. A crash is covered by the unit's ExecStopPost, because nothing
    // else picks up after a daemon that owns the controls alone.
    SignalPipe signals_;
    QObject::connect(&signals_, &SignalPipe::signalled, &daemon, [&daemon](int number) {
        if (number == SIGHUP) {
            daemon.handleCommand(QStringLiteral("reload"));
            return;
        }
        daemon.shutdown();
    });
    if (!signals_.install())
        qWarning() << "could not install signal handlers; relying on ExecStopPost to restore";

    QObject::connect(&daemon, &Daemon::finished, &app, &QCoreApplication::quit);

    QString error;
    if (!daemon.start(&error)) {
        qCritical().noquote() << "cannot start:" << error;
        leave(1);
    }

    // The control surface. A bus that is not there yet is not a reason to stop
    // managing the machine, so this keeps trying in the background; the one refusal
    // is another daemon already answering on the name, because two owners of one
    // control surface is what this architecture forbids.
    ControlBus::Options busOptions;
    busOptions.busAddress = parser.value(busOption);
    ControlBus control(daemon, busOptions);
    if (!control.start(&error)) {
        qCritical().noquote() << "cannot start:" << error;
        daemon.shutdown();
        leave(1);
    }
    if (control.owned())
        qInfo().noquote() << "answering on" << control.name();

    // Compatibility, not the primary interface: a bus that cannot be reached
    // costs the desktop panel, not thermal management.
    std::unique_ptr<DBusService> dbus;
    if (!parser.isSet(noDbusOption)) {
        dbus = std::make_unique<DBusService>(daemon, DBusService::Options {});
        QString dbusError;
        if (dbus->start(&dbusError))
            qInfo().noquote() << "answering on" << dbus->claimedNames().join(QStringLiteral(", "));
        else
            qWarning().noquote() << "no D-Bus names claimed:" << dbusError;
    }

    leave(app.exec());
}
