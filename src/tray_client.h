// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "brightness_map.h"
#include "metrics_ring.h"
#include "setpoints.h"

#include "bus_client.h"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

// The console's end of the daemon's interface.
//
// The daemon publishes what it is doing, so nothing here asks once a second: the
// readings arrive as they are taken, which is what live tuning needs — the useful
// question while turning a slider is "does this sound acceptable right now", and that
// needs the current fan speed beside it.
//
// What leaves this class is what left it before: a `Status` the window paints itself
// from, and the daemon's own reply line for every command. The window does not know
// there is a bus underneath.
class TrayClient : public QObject {
    Q_OBJECT

public:
    // A reading the daemon reported as absent keeps its sentinel, so the
    // window can show a dash rather than an invented zero.
    static constexpr double kNoReading = -1000.0;

    struct Status {
        bool valid = false;
        QString modeId;
        // Ordered coolest first, as the daemon reports them.
        QStringList modeIds;
        QStringList modeLabels;
        bool liveTuning = false;
        bool guardEngaged = false;
        bool fanForced = false;
        bool writeMismatch = false;
        bool sensorsDegraded = false;
        bool onAc = false;

        // What was applied on the last tick, and what the mode or the live
        // overlay asks for. They differ while the emergency guard is clamping.
        Setpoints applied;
        Setpoints requested;

        double packageC = kNoReading;
        double boardC = kNoReading;
        double nvmeC = kNoReading;
        double fanRpm = kNoReading;
        double packageW = kNoReading;
        double coreW = kNoReading;
        double uncoreW = kNoReading;
        double cpuMhz = kNoReading;
        double gpuMhz = kNoReading;
        double load1 = kNoReading;

        // The backlight side. Reported alongside the thermal state and never
        // mixed with it: following the ambient light sensor is a separate
        // function with its own profile and its own switch.
        bool brightnessAvailable = false;
        bool autoBrightness = false;
        double brightness = kNoReading;
        // The level with the light's contribution taken out. Shown because it
        // is what the automation works from, and a number that differs from the
        // one just set looks like a defect until it is named.
        double brightnessBase = kNoReading;
        // How far the automation may move and the level nothing goes below.
        // Defaults match the built-in settings so the sliders have somewhere to
        // sit before the first answer arrives.
        int brightnessResponsePct = kBrightnessDefaultResponsePct;
        int brightnessFloorPct = kBrightnessDefaultFloorPct;
        int brightnessCeilingPct = kBrightnessDefaultCeilingPct;
        // How the telemetry is kept. Shown on the settings tab beside the reset,
        // which is the other control there that reaches the daemon.
        // The external monitors, as the daemon reported them. A level below zero
        // is a monitor that stopped answering: switched off, or its input moved to
        // another machine.
        struct Monitor {
            QString id;
            QString model;
            QString connector;
            int level = -1;
        };
        QList<Monitor> monitors;

        int retentionDays = kRetentionDefaultDays;
        bool metricsEnabled = true;
        // The band those two produce, which is what the window shows: "between
        // 50 and 100 per cent" is a sentence, "a response of thirty" is not.
        int brightnessWindowLow = kBrightnessDefaultFloorPct;
        int brightnessWindowHigh = 100;
    };

    // What the daemon has saved, as opposed to what it is running. Fetched on
    // demand rather than polled: definitions change when somebody changes
    // them, and a console that re-read them once a second would fight whoever
    // was typing into a field.
    struct ModeDefinition {
        QString id;
        QString label;
        Setpoints setpoints;
    };

    struct Configuration {
        bool valid = false;
        // Where the daemon keeps it, and whether anything has been changed yet.
        QString path;
        bool stored = false;
        QString defaultModeId;
        QList<ModeDefinition> modes;
    };

    // Empty means the system bus. An address is how the suite runs against a daemon
    // of its own without touching the bus the desktop is on.
    explicit TrayClient(QString busAddress = QString(), QObject *parent = nullptr);

    // Reads everything once and listens from then on. The daemon may restart under an
    // open console, and that is noticed by the name changing hands rather than by
    // asking.
    void start();
    void stop();

    // Sends one command in the vocabulary of the command line and returns the
    // daemon's reply line: "ok", "ok <what happened>" or "err <why>". Empty when
    // there is no daemon.
    QString send(const QString &command);

    // Sends a command and immediately refreshes, so the window never shows a
    // state the daemon has already left.
    QString sendAndRefresh(const QString &command);

    const Status &status() const { return m_status; }
    bool connected() const { return m_connected; }

    // Asks the daemon what it has saved. Invalid when it did not answer.
    Configuration fetchConfiguration();

    // Parses a status reply. Separate from the bus so that every field mapping is
    // testable without a running daemon.
    static Status parseStatus(const QJsonObject &object);
    static Configuration parseConfiguration(const QJsonObject &object);

public slots:
    // Reads everything the daemon publishes, now. Used after a command, because a
    // window that has just been told "ok" should show the result of it rather than
    // wait to be told about it.
    void poll();

signals:
    void statusChanged(const TrayClient::Status &status);
    void connectionChanged(bool connected);

private:
    // Emits connectionChanged only on a real transition, so the interface is
    // not repainted once a second while the daemon is absent.
    void setConnected(bool connected);

    // Merges what the daemon announced into what is already known, and rebuilds the
    // status from it: a change carries only the keys that moved.
    void merge(const QVariantMap &changed);

    BusClient m_bus;
    // Everything the daemon publishes, as last read or announced.
    QVariantMap m_properties;
    Status m_status;
    bool m_connected = false;
};

Q_DECLARE_METATYPE(TrayClient::Status)
