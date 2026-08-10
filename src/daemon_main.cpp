// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "daemon.h"
#include "dbus_service.h"
#include "signal_pipe.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>

#include <csignal>
#include <memory>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("sombrad"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
            QStringLiteral("Thermal and power daemon for the ASUS Zenbook 14 UX3405MA."));
    parser.addHelpOption();
    parser.addVersionOption();

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
    const QCommandLineOption socketOption(QStringLiteral("socket"),
                                          QStringLiteral("Control socket."),
                                          QStringLiteral("path"), defaults.socketPath);
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
            QStringLiteral("Do not claim the desktop power-profile names. The control socket is "
                           "the primary interface; the D-Bus names are compatibility."));
    parser.addOptions({ rootOption, configOption, stateOption, ringOption, socketOption,
                        capacityOption, ticksOption, intervalOption, noDbusOption });
    parser.process(app);

    DaemonOptions options;
    options.sysfsRoot = parser.value(rootOption);
    options.configPath = parser.value(configOption);
    options.statePath = parser.value(stateOption);
    options.ringPath = parser.value(ringOption);
    options.socketPath = parser.value(socketOption);
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
        return 1;
    }

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

    return app.exec();
}
