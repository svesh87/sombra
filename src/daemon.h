// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "actuator.h"
#include "backlight.h"
#include "brightness_map.h"
#include "control_socket.h"
#include "daemon_state.h"
#include "light_sensor.h"
#include "metrics_ring.h"
#include "modes.h"
#include "power_source.h"
#include "sensors.h"
#include "sysfs.h"

#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>

#include <memory>
#include <optional>

struct DaemonOptions {
    // Only the kernel control surface is rooted. Configuration, state, the
    // ring and the socket are ordinary paths, given in full so that a test run
    // and a service run differ only in their arguments.
    QString sysfsRoot = QStringLiteral("/");
    QString configPath = QStringLiteral("/etc/ombra/modes.conf");
    QString statePath = QStringLiteral("/var/lib/ombra/state");
    QString ringPath = QStringLiteral("/var/lib/ombra/metrics.ring");
    QString socketPath = QStringLiteral("/run/ombra/control.sock");
    quint64 ringCapacity = kRingCapacity;
    int intervalMs = 1000;
    // The backlight fade runs far faster than the sampling loop, because a
    // correct step applied in one write still reads as a defect.
    int fadeIntervalMs = kBrightnessFadeIntervalMs;
    int flushSeconds = 60;
    // Stop after this many ticks. Zero runs until signalled; a positive value
    // is what lets the test suite drive the whole daemon deterministically.
    int maxTicks = 0;
};

// The 1 Hz loop.
//
// Each tick reads the sensors, decides the setpoints, lets the emergency guard
// lower them, applies them and records everything — readings and setpoints
// together — as one sample. The daemon does not adjust setpoints towards a
// target and does not change mode by itself: mode selection is a deliberate
// act, and the guard is an interlock rather than a controller.
class Daemon : public QObject {
    Q_OBJECT

public:
    explicit Daemon(DaemonOptions options, QObject *parent = nullptr);
    ~Daemon() override;

    bool start(QString *error = nullptr);

    // Restores the hardware and stops the loop. Every exit path the daemon
    // controls goes through here.
    void shutdown();

    // One iteration. Public so tests can step the daemon without a clock.
    void tick();

    // One step of the backlight fade. Public for the same reason as tick():
    // it is driven by its own timer in service, and by the test directly.
    void fadeBrightness();
    bool fading() const { return m_fadeTargetRaw >= 0; }

    QString handleCommand(const QString &line);

    const Sample &lastSample() const { return m_lastSample; }
    const QString &activeModeId() const { return m_state.mode; }
    const ModeTable &modes() const { return m_modes; }
    // The setpoints the mode or the live overlay asks for, before the guard.
    Setpoints requestedSetpoints() const;
    bool liveTuning() const { return m_live.has_value(); }
    quint64 tickCount() const { return m_ticks; }
    const DaemonState &state() const { return m_state; }

signals:
    void finished();
    // Emitted after every applied change of mode, so the D-Bus layer can
    // announce it without polling.
    void modeChanged(const QString &id);

private:
    // Applies the current setpoints through the guard, without recording a
    // sample. Used at startup, where an apply must not skip the interlock but
    // there is no tick to attribute a sample to.
    void applyNow();
    void loadConfiguration();
    void loadState();
    void saveState();
    QString applyMode(const QString &id);
    QString applyLiveSetting(const QStringList &assignments);
    QString applyBrightnessCommand(const QStringList &parts);
    QString statusJson() const;

    // The backlight path. Kept apart from the setpoint path on purpose: the
    // thermal mode must not be able to reach the backlight, and the backlight
    // must not be able to reach the thermal controls, not even by accident.
    void updateBrightness(Sample &sample);
    // A hand correction is an instruction, and it is read before anything is
    // computed: our own write would otherwise overwrite it before it was seen.
    void noticeManualBrightness();
    void startBrightnessFade(int targetPercent);
    void stopBrightnessFade();

    DaemonOptions m_options;
    Sysfs m_sysfs;
    SensorReader m_sensors;
    Actuator m_actuator;
    PowerSource m_powerSource;
    LightSensor m_lightSensor;
    Backlight m_backlight;
    MetricsRing m_ring;
    ModeTable m_modes;
    BrightnessProfileTable m_brightnessProfiles;
    std::unique_ptr<ControlSocket> m_socket;
    QFileSystemWatcher m_watcher;
    QTimer m_timer;
    QTimer m_fadeTimer;
    QElapsedTimer m_monotonic;

    DaemonState m_state;
    BrightnessState m_brightness;
    // Target of the fade in the panel's own units, or -1 when none is running.
    int m_fadeTargetRaw = -1;
    int m_fadeStepRaw = 1;

    std::optional<Setpoints> m_live;
    // Held apart from live tuning because it is a different kind of thing: no
    // mode may ask for the fan, so an explicit fan command is not a setpoint
    // the mode could have carried.
    std::optional<int> m_fanOverride;
    GuardState m_guard;
    Mark m_pendingMark = Mark::None;
    Sample m_lastSample {};
    Setpoints m_lastApplied;
    quint64 m_ticks = 0;
    int m_sinceFlush = 0;
    bool m_running = false;
};
