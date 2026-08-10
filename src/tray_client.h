// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "brightness_map.h"
#include "setpoints.h"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

// The tray's end of the control socket.
//
// One line out, one line in. The tray polls status at the daemon's own rate,
// because live tuning is judged against numbers that move: the useful question
// while turning a slider is "does this sound acceptable right now", and that
// needs the current fan speed beside it.
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

    explicit TrayClient(QString socketPath, QObject *parent = nullptr);

    // Starts polling. Reconnects on its own: the daemon may restart, the tray
    // does not.
    void start(int intervalMs = 1000);
    void stop();

    // Sends one command and returns the daemon's reply line, or an empty
    // string when the daemon is unreachable. Blocking with a short timeout,
    // which is what a single-line exchange over a local socket warrants.
    QString send(const QString &command);

    // Sends a command and immediately refreshes, so the window never shows a
    // state the daemon has already left.
    QString sendAndRefresh(const QString &command);

    const Status &status() const { return m_status; }
    bool connected() const { return m_connected; }
    const QString &socketPath() const { return m_socketPath; }

    // Asks the daemon what it has saved. Invalid when it did not answer.
    Configuration fetchConfiguration();

    // Parses a status reply. Separate from the socket so that every field
    // mapping is testable without a running daemon.
    static Status parseStatus(const QJsonObject &object);
    static Configuration parseConfiguration(const QJsonObject &object);

public slots:
    void poll();

signals:
    void statusChanged(const TrayClient::Status &status);
    void connectionChanged(bool connected);

private:
    // Emits connectionChanged only on a real transition, so the interface is
    // not repainted once a second while the daemon is absent.
    void setConnected(bool connected);

    QString m_socketPath;
    QTimer m_timer;
    Status m_status;
    bool m_connected = false;
};

Q_DECLARE_METATYPE(TrayClient::Status)
