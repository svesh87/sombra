// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "daemon.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace {

QJsonValue temperature(int16_t deci)
{
    if (deci == kAbsentTemp)
        return QJsonValue();
    return QJsonValue(deci / 10.0);
}

QJsonValue centiwatts(uint16_t value)
{
    if (value == kAbsentValue)
        return QJsonValue();
    return QJsonValue(value / 100.0);
}

QJsonValue plain(uint16_t value)
{
    if (value == kAbsentValue)
        return QJsonValue();
    return QJsonValue(static_cast<int>(value));
}

} // namespace

Daemon::Daemon(DaemonOptions options, QObject *parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_sysfs(m_options.sysfsRoot)
    , m_sensors(m_sysfs)
    , m_actuator(m_sysfs)
    , m_powerSource(m_sysfs)
    , m_lightSensor(m_sysfs)
    , m_backlight(m_sysfs)
    , m_modes(ModeTable::builtinDefaults())
    , m_brightnessSettings(builtinBrightnessSettings())
{
    m_timer.setInterval(m_options.intervalMs);
    connect(&m_timer, &QTimer::timeout, this, &Daemon::tick);

    m_fadeTimer.setInterval(m_options.fadeIntervalMs);
    connect(&m_fadeTimer, &QTimer::timeout, this, &Daemon::fadeBrightness);

    // Editors replace a configuration file rather than rewriting it, which
    // drops the watch, so the path is re-added on every notification.
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &path) {
        loadConfiguration();
        if (!m_watcher.files().contains(path) && QFileInfo::exists(path))
            m_watcher.addPath(path);
    });
}

Daemon::~Daemon()
{
    shutdown();
}

bool Daemon::start(QString *error)
{
    m_sensors.resolve();
    m_actuator.resolve();
    m_powerSource.resolve();
    m_lightSensor.resolve();
    m_backlight.resolve();

    if (!m_actuator.missingNodes().isEmpty()) {
        // Missing nodes are graceful degradation with a log line, not a crash:
        // hwmon layout and kernel exports differ between machines.
        qWarning().noquote() << "control nodes absent, those controls are inactive:"
                             << m_actuator.missingNodes().join(QStringLiteral(", "));
    }

    if (!m_actuator.hasAnyControl()) {
        if (error) {
            *error = QStringLiteral("no thermal control nodes under %1")
                             .arg(m_sysfs.absolute(QString()));
        }
        return false;
    }

    // A live socket means another daemon already owns the control surface.
    // Two writers is the one thing this architecture forbids, so this is the
    // only startup condition worth refusing over.
    if (ControlSocket::isServerLive(m_options.socketPath)) {
        if (error) {
            *error = QStringLiteral("another daemon is answering on %1")
                             .arg(m_options.socketPath);
        }
        return false;
    }

    // Telemetry and the control channel are both secondary to keeping the
    // machine inside its thermal envelope. Neither failing is a reason to
    // leave the firmware's own 200 W limit standing, so both degrade to a log
    // line.
    QString ringError;
    if (!m_ring.open(m_options.ringPath, m_options.ringCapacity, &ringError))
        qWarning().noquote() << "telemetry disabled:" << ringError;
    // Said out loud: samples carried across a change of format, or a file this
    // build could not read and kept instead of deleting.
    if (!m_ring.note().isEmpty())
        qInfo().noquote() << m_ring.note();

    loadConfiguration();
    loadState();

    m_socket = std::make_unique<ControlSocket>(
            [this](const QString &line) { return handleCommand(line); }, this);
    QString socketError;
    if (!m_socket->listen(m_options.socketPath, &socketError))
        qWarning().noquote() << "control socket unavailable:" << socketError;

    // The charge threshold lives in sysfs and is lost across a reboot, and
    // holding it is an obligation inherited together with sole ownership of
    // the power controls. Nothing else will re-apply it.
    if (m_state.chargeLimit != DaemonState::kChargeLimitUnset
        && !m_powerSource.setChargeLimit(m_state.chargeLimit)) {
        qWarning().noquote() << "charge threshold not restored: charge control is unavailable";
    }

    m_monotonic.start();
    m_running = true;
    applyMode(m_state.mode);
    // The saved mode takes effect at boot rather than one tick later. A
    // machine that spends its first second under the firmware's own 200 W
    // limit has already lost the argument, and the guard sees a reading first
    // so that a restart under load cannot un-throttle a hot package.
    applyNow();
    m_timer.start();
    return true;
}

void Daemon::applyNow()
{
    const Sample reading = m_sensors.read(
            static_cast<uint32_t>(QDateTime::currentSecsSinceEpoch()), m_monotonic.elapsed());
    m_lastApplied = applyGuard(reading, requestedSetpoints(), m_guard);
    m_actuator.apply(m_lastApplied);
}

void Daemon::shutdown()
{
    if (!m_running)
        return;
    m_running = false;
    m_timer.stop();
    stopBrightnessFade();
    if (m_socket)
        m_socket->close();
    m_actuator.restore();
    m_ring.flush();
    m_ring.close();
    emit finished();
}

void Daemon::loadConfiguration()
{
    // No file means nothing has been changed yet, and the built-in values are
    // the answer. They are also what a reset returns to, so the two paths agree
    // by construction.
    ModeTable table;
    QStringList problems;
    if (table.load(m_options.configPath, &problems)) {
        m_modes = table;
    } else {
        m_modes = ModeTable::builtinDefaults();
    }

    // The same file, a separate section: the backlight shares the settings file
    // with the modes and nothing else.
    BrightnessSettings brightness;
    if (loadBrightnessSettings(m_options.configPath, &brightness, &problems))
        m_brightnessSettings = brightness;
    else
        m_brightnessSettings = builtinBrightnessSettings();

    for (const QString &problem : problems)
        qWarning().noquote() << "configuration:" << problem;

    if (!m_modes.find(m_state.mode))
        m_state.mode = m_modes.defaultModeId();

    if (!m_watcher.files().contains(m_options.configPath)
        && QFileInfo::exists(m_options.configPath)) {
        m_watcher.addPath(m_options.configPath);
    }
}

void Daemon::loadState()
{
    QStringList problems;
    m_state = DaemonState::load(m_options.statePath, &problems);
    for (const QString &problem : problems)
        qWarning().noquote() << "state:" << problem;

    // The middle mode is the working one and is what the machine boots into.
    if (!m_modes.find(m_state.mode))
        m_state.mode = m_modes.defaultModeId();

    // The panel's current level is the first base. The machine is already set
    // up the way its owner likes it, and taking that as the starting point is
    // more honest than imposing a number of our own.
    if (const auto raw = m_backlight.readRaw()) {
        m_backlight.noteObserved(*raw);
        if (m_state.brightnessBase == DaemonState::kBrightnessBaseUnset)
            m_state.brightnessBase = m_backlight.toPercent(*raw);
    }
}

void Daemon::saveState()
{
    if (!m_state.save(m_options.statePath))
        qWarning().noquote() << "state file could not be written:" << m_options.statePath;
}

Setpoints Daemon::requestedSetpoints() const
{
    Setpoints points;
    if (m_live) {
        points = *m_live;
    } else if (const Mode *mode = m_modes.find(m_state.mode)) {
        points = mode->setpoints;
    } else if (const Mode *fallback = m_modes.mostConservative()) {
        points = fallback->setpoints;
    }
    return points;
}

QString Daemon::applyMode(const QString &id)
{
    const Mode *mode = m_modes.find(id);
    if (!mode)
        return QStringLiteral("err unknown mode %1").arg(id);
    // Selecting a mode is the deliberate act that cancels live tuning; the last
    // command wins.
    m_live.reset();
    m_state.mode = id;
    saveState();
    emit modeChanged(id);
    return QStringLiteral("ok %1").arg(id);
}

void Daemon::noticeManualBrightness()
{
    const auto current = m_backlight.readRaw();
    if (!current)
        return;

    const auto known = m_backlight.lastKnownRaw();
    if (!known || *known == *current) {
        // Either the level is the one we put there, or this is the first
        // reading and there is nothing to compare it against.
        m_backlight.noteObserved(*current);
        return;
    }
    m_backlight.noteObserved(*current);

    // Somebody else set this level, and on this machine that somebody is the
    // person on the brightness keys. What they asked for is an instruction, so
    // the base is re-derived from it: the level they chose, with the light's
    // contribution of this moment taken back out. Coming back to the same room
    // then comes back to the same level.
    //
    // The floor applies to them as well. It is the one bound that binds
    // everybody, because a screen driven into unreadable is a failure whoever
    // did the driving — and a level that was overridden must not become a base
    // nobody can see on screen.
    const int asked = m_backlight.toPercent(*current);
    const int applied = std::clamp(asked, m_brightnessSettings.floorPct, 100);
    stopBrightnessFade();
    m_brightness.step = BrightnessState::kUnset;
    m_brightness.pending = BrightnessState::kUnset;
    m_brightness.pendingTicks = 0;
    if (applied != asked) {
        m_backlight.writeRaw(m_backlight.toRaw(applied));
        if (const auto raw = m_backlight.readRaw())
            m_backlight.noteObserved(*raw);
    }

    const int base = brightnessBaseFor(m_brightnessSettings, applied, m_lightSensor.read());
    if (m_state.brightnessBase != base) {
        m_state.brightnessBase = base;
        saveState();
    }
}

QString Daemon::applyBrightnessLevel(int percent)
{
    if (!m_backlight.available())
        return QStringLiteral("err the backlight is not available");
    const int applied = std::clamp(percent, m_brightnessSettings.floorPct, 100);
    stopBrightnessFade();
    if (!m_backlight.writeRaw(m_backlight.toRaw(applied)))
        return QStringLiteral("err the backlight could not be written");
    if (const auto raw = m_backlight.readRaw())
        m_backlight.noteObserved(*raw);

    // The same path a hand correction takes: a level is a level whoever set it,
    // and the base follows from it the same way.
    m_brightness.step = BrightnessState::kUnset;
    m_brightness.pending = BrightnessState::kUnset;
    m_brightness.pendingTicks = 0;
    m_state.brightnessBase = brightnessBaseFor(m_brightnessSettings, applied,
                                               m_lightSensor.read());
    saveState();
    return QStringLiteral("ok %1").arg(applied);
}

void Daemon::updateBrightness(Sample &sample)
{
    sample.light = kAbsentLight;
    sample.brightness = kAbsentPercent;
    sample.brightnessBase = kAbsentPercent;
    // Zero is a legal response and a legal floor, so absence needs its own
    // marker here as much as it does for the readings.
    sample.brightnessResponse = kAbsentPercent;
    sample.brightnessFloor = kAbsentPercent;

    if (!m_backlight.available())
        return;

    // Recorded whether or not the automation is on: this is what the next move
    // would have been allowed to do.
    sample.brightnessResponse = static_cast<uint8_t>(m_brightnessSettings.responsePct);
    sample.brightnessFloor = static_cast<uint8_t>(m_brightnessSettings.floorPct);

    // Read the panel before deciding anything: our own write would overwrite a
    // hand correction before it had been seen.
    noticeManualBrightness();

    if (const auto percent = m_backlight.readPercent())
        sample.brightness = static_cast<uint8_t>(*percent);
    if (m_state.brightnessBase != DaemonState::kBrightnessBaseUnset)
        sample.brightnessBase = static_cast<uint8_t>(m_state.brightnessBase);

    const auto light = m_lightSensor.read();
    if (light)
        sample.light = *light;

    // Switched off, the automation does not write the backlight at all. Not
    // "moves it less" — at all.
    if (!m_state.autoBrightness)
        return;
    sample.flags |= FlagAutoBrightness;

    m_brightness.base = m_state.brightnessBase;
    const BrightnessDecision decision =
            decideBrightness(m_brightnessSettings, light, m_brightness);
    if (decision.move)
        startBrightnessFade(decision.target);
}

void Daemon::startBrightnessFade(int targetPercent)
{
    const auto currentRaw = m_backlight.readRaw();
    if (!currentRaw)
        return;
    const int targetRaw = m_backlight.toRaw(targetPercent);
    if (targetRaw == *currentRaw)
        return;

    m_fadeTargetRaw = targetRaw;
    // Whatever the distance, the move takes about kBrightnessFadeSteps writes,
    // and never less than one unit of the panel's own scale per write.
    const int distance = std::abs(targetRaw - *currentRaw);
    m_fadeStepRaw = std::max(1, (distance + kBrightnessFadeSteps - 1) / kBrightnessFadeSteps);
    m_fadeTimer.start();
}

void Daemon::stopBrightnessFade()
{
    m_fadeTimer.stop();
    m_fadeTargetRaw = -1;
}

void Daemon::fadeBrightness()
{
    if (m_fadeTargetRaw < 0) {
        m_fadeTimer.stop();
        return;
    }
    const auto currentRaw = m_backlight.readRaw();
    if (!currentRaw) {
        stopBrightnessFade();
        return;
    }
    // Somebody moved the backlight while the fade was running, and the person
    // wins immediately rather than after the fade has finished arguing.
    if (const auto known = m_backlight.lastKnownRaw(); known && *known != *currentRaw) {
        stopBrightnessFade();
        return;
    }

    const int next = *currentRaw < m_fadeTargetRaw
            ? std::min(m_fadeTargetRaw, *currentRaw + m_fadeStepRaw)
            : std::max(m_fadeTargetRaw, *currentRaw - m_fadeStepRaw);
    if (!m_backlight.writeRaw(next)) {
        stopBrightnessFade();
        return;
    }
    if (next == m_fadeTargetRaw)
        stopBrightnessFade();
}

QString Daemon::applyBrightnessCommand(const QStringList &parts)
{
    if (parts.isEmpty())
        return QStringLiteral("err brightness needs on, off or profile");
    const QString what = parts.first().toLower();

    if (what == QLatin1String("on") || what == QLatin1String("off")) {
        const bool wanted = what == QLatin1String("on");
        if (m_state.autoBrightness != wanted) {
            m_state.autoBrightness = wanted;
            saveState();
        }
        // Neither switching on nor switching off moves the backlight. A jump
        // in answer to a toggle is the same defect as one in answer to a
        // profile switch. Switching on only adopts a position; the next move
        // waits for the light itself to change.
        stopBrightnessFade();
        m_brightness.step = BrightnessState::kUnset;
        m_brightness.pending = BrightnessState::kUnset;
        m_brightness.pendingTicks = 0;
        return QStringLiteral("ok");
    }

    if (what == QLatin1String("level")) {
        if (parts.size() < 2)
            return QStringLiteral("err brightness level needs a percentage");
        bool ok = false;
        const int percent = parts.at(1).toInt(&ok);
        if (!ok)
            return QStringLiteral("err brightness level is not a number");
        if (percent < 0 || percent > 100)
            return QStringLiteral("err brightness level is outside 0..100");
        return applyBrightnessLevel(percent);
    }

    if (what == QLatin1String("set")) {
        // These are settings, not an overlay: what the slider leaves behind is
        // what the automation uses from then on, which is how every desktop
        // treats a brightness control. Saving them is a separate command so
        // that dragging a slider does not write a file thirty times.
        BrightnessSettings edited = m_brightnessSettings;
        QStringList clamped;
        for (const QString &assignment : parts.mid(1)) {
            const int eq = assignment.indexOf(QLatin1Char('='));
            if (eq <= 0)
                return QStringLiteral("err malformed assignment %1").arg(assignment);
            QString error;
            if (!applyBrightnessSetting(edited, assignment.left(eq).trimmed().toLower(),
                                        assignment.mid(eq + 1).trimmed(), &error, &clamped)) {
                return QStringLiteral("err ") + error;
            }
        }
        m_brightnessSettings = edited;

        // A changed floor can leave the panel below it, and the floor binds
        // everybody. Re-deriving the base keeps the level the person is looking
        // at meaning what it says.
        if (const auto percent = m_backlight.readPercent()) {
            if (*percent < edited.floorPct)
                applyBrightnessLevel(edited.floorPct);
            else
                m_state.brightnessBase =
                        brightnessBaseFor(edited, *percent, m_lightSensor.read());
        }
        m_brightness.step = BrightnessState::kUnset;
        m_brightness.pending = BrightnessState::kUnset;
        m_brightness.pendingTicks = 0;
        if (clamped.isEmpty())
            return QStringLiteral("ok");
        return QStringLiteral("ok ") + clamped.join(QStringLiteral("; "));
    }

    return QStringLiteral("err brightness takes on, off, level or set");
}

QString Daemon::applyLiveSetting(const QStringList &assignments)
{
    Setpoints points = requestedSetpoints();
    for (const QString &assignment : assignments) {
        const int eq = assignment.indexOf(QLatin1Char('='));
        if (eq <= 0)
            return QStringLiteral("err malformed assignment %1").arg(assignment);
        const QString key = assignment.left(eq).trimmed().toLower();
        const QString value = assignment.mid(eq + 1).trimmed();
        bool numberOk = false;
        const int number = value.toInt(&numberOk);

        if (key == QLatin1String("pl1")) {
            if (!numberOk)
                return QStringLiteral("err pl1 is not a number");
            points.pl1W = std::clamp(number, 1, kMaxConfirmedPl1W);
        } else if (key == QLatin1String("pl2")) {
            if (!numberOk)
                return QStringLiteral("err pl2 is not a number");
            points.pl2W = std::clamp(number, 1, kMaxConfirmedPl2W);
        } else if (key == QLatin1String("max_perf_pct")) {
            if (!numberOk)
                return QStringLiteral("err max_perf_pct is not a number");
            points.maxPerfPct = std::clamp(number, kMinPerfPct, 100);
        } else if (key == QLatin1String("gt_max_mhz")) {
            if (!numberOk)
                return QStringLiteral("err gt_max_mhz is not a number");
            points.gtMaxMhz = std::clamp(number, kMinGtMhz, kMaxGtMhz);
        } else if (key == QLatin1String("turbo")) {
            points.noTurbo = value != QLatin1String("on") && value != QLatin1String("1");
        } else if (key == QLatin1String("hwp_boost")) {
            points.hwpBoost = value == QLatin1String("on") || value == QLatin1String("1");
        } else if (key == QLatin1String("epp")) {
            const Epp epp = eppFromString(value);
            if (epp == Epp::Unknown)
                return QStringLiteral("err unknown epp %1").arg(value);
            points.epp = epp;
        } else if (key == QLatin1String("profile")) {
            const Profile profile = profileFromString(value);
            if (profile == Profile::Unknown)
                return QStringLiteral("err unknown profile %1").arg(value);
            points.profile = profile;
        } else if (key == QLatin1String("fan")) {
            if (value == QLatin1String("max")) {
                points.fanMode = 0;
            } else if (value == QLatin1String("auto")) {
                points.fanMode = 2;
            } else {
                return QStringLiteral("err fan takes max or auto");
            }
        } else {
            return QStringLiteral("err unknown setting %1").arg(key);
        }
    }
    // Live values apply immediately and are never written to the settings file
    // by themselves. Keeping one is a separate, deliberate command —
    // `config save live to <id>` — which names the mode it lands in.
    m_live = points;
    return QStringLiteral("ok");
}

QString Daemon::commitTables(const ModeTable &modes, const BrightnessSettings &brightness,
                             const QStringList &clamped)
{
    QString error;
    if (!settings_file::save(m_options.configPath, modes, brightness, &error))
        return QStringLiteral("err ") + error;

    m_modes = modes;
    m_brightnessSettings = brightness;
    // An edit can rename nothing but can move a mode along the table, and a
    // selection that no longer exists would leave the machine unmanaged.
    if (!m_modes.find(m_state.mode))
        m_state.mode = m_modes.defaultModeId();

    // The file did not exist until the first change, so this is where the
    // watch usually starts.
    if (!m_watcher.files().contains(m_options.configPath)
        && QFileInfo::exists(m_options.configPath)) {
        m_watcher.addPath(m_options.configPath);
    }

    // The definition that changed may be the one in force, and waiting for the
    // next tick to apply it would make an edit look like it did nothing.
    if (m_running)
        applyNow();

    if (clamped.isEmpty())
        return QStringLiteral("ok");
    // Clamping is reported rather than silent: otherwise somebody spends an
    // evening tuning a number that never took effect.
    return QStringLiteral("ok ") + clamped.join(QStringLiteral("; "));
}

QString Daemon::configJson() const
{
    QJsonArray modeList;
    for (const Mode &mode : m_modes.modes()) {
        QJsonObject entry;
        entry[QStringLiteral("id")] = mode.id;
        entry[QStringLiteral("label")] = mode.label;
        entry[QStringLiteral("platform_profile")] = profileToString(mode.setpoints.profile);
        entry[QStringLiteral("pl1_w")] = mode.setpoints.pl1W;
        entry[QStringLiteral("pl2_w")] = mode.setpoints.pl2W;
        entry[QStringLiteral("max_perf_pct")] = mode.setpoints.maxPerfPct;
        entry[QStringLiteral("turbo")] = !mode.setpoints.noTurbo;
        entry[QStringLiteral("epp")] = eppToString(mode.setpoints.epp);
        entry[QStringLiteral("hwp_dynamic_boost")] = mode.setpoints.hwpBoost;
        entry[QStringLiteral("gt_max_mhz")] = mode.setpoints.gtMaxMhz;
        modeList.append(entry);
    }

    QStringList points;
    for (const BrightnessPoint &point : m_brightnessSettings.curve) {
        points.append(QStringLiteral("%1:%2")
                              .arg(point.light / 1000.0, 0, 'g', 6)
                              .arg(point.position));
    }
    QJsonObject brightness;
    brightness[QStringLiteral("response_pct")] = m_brightnessSettings.responsePct;
    brightness[QStringLiteral("floor_pct")] = m_brightnessSettings.floorPct;
    // Lux, like everywhere a light level leaves this process.
    brightness[QStringLiteral("curve")] = points.join(QStringLiteral(", "));

    QJsonObject root;
    // The path is part of the answer: tooling that reports what it changed has
    // to be able to say where, and a person asking "what is saved" is asking
    // about a file.
    root[QStringLiteral("path")] = m_options.configPath;
    root[QStringLiteral("stored")] = QFileInfo::exists(m_options.configPath);
    root[QStringLiteral("default_mode")] = m_modes.defaultModeId();
    root[QStringLiteral("modes")] = modeList;
    root[QStringLiteral("brightness")] = brightness;
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QString Daemon::configModeCommand(const QStringList &parts)
{
    if (parts.isEmpty())
        return QStringLiteral("err config set mode needs a mode name");
    const QString id = parts.first();
    const Mode *existing = m_modes.find(id);
    if (!existing) {
        // Modes are edited, never created: the desktop panel has three slots,
        // and a fourth definition would live where the panel cannot reach it.
        QStringList known;
        for (const Mode &mode : m_modes.modes())
            known.append(mode.id);
        return QStringLiteral("err unknown mode %1; there are %2")
                .arg(id, known.join(QStringLiteral(", ")));
    }
    if (parts.size() < 2)
        return QStringLiteral("err config set mode needs at least one assignment");

    Mode edited = *existing;
    QStringList clamped;
    for (const QString &assignment : parts.mid(1)) {
        const int eq = assignment.indexOf(QLatin1Char('='));
        if (eq <= 0)
            return QStringLiteral("err malformed assignment %1").arg(assignment);
        QString error;
        if (!applyModeSetting(edited, assignment.left(eq).trimmed().toLower(),
                              assignment.mid(eq + 1).trimmed(), &error, &clamped)) {
            return QStringLiteral("err ") + error;
        }
    }
    normalizeMode(edited, &clamped);

    ModeTable modes = m_modes;
    modes.replace(edited);
    return commitTables(modes, m_brightnessSettings, clamped);
}

QString Daemon::configBrightnessCommand(const QStringList &parts)
{
    if (parts.isEmpty())
        return QStringLiteral("err config set brightness needs at least one assignment");

    BrightnessSettings edited = m_brightnessSettings;
    QStringList clamped;
    for (const QString &assignment : parts) {
        const int eq = assignment.indexOf(QLatin1Char('='));
        if (eq <= 0)
            return QStringLiteral("err malformed assignment %1").arg(assignment);
        QString error;
        if (!applyBrightnessSetting(edited, assignment.left(eq).trimmed().toLower(),
                                    assignment.mid(eq + 1).trimmed(), &error, &clamped)) {
            return QStringLiteral("err ") + error;
        }
    }
    return commitTables(m_modes, edited, clamped);
}

QString Daemon::configSaveCommand(const QStringList &parts)
{
    // "config save live to <id>". The destination is named rather than
    // assumed: saving into whichever mode happens to be selected is the kind of
    // guess that loses an evening's tuning.
    //
    // There is no brightness half any more. The backlight numbers are settings
    // rather than an overlay — what the slider leaves behind is what the
    // automation uses — and `config set brightness` writes them.
    if (parts.size() != 3 || parts.first().toLower() != QLatin1String("live")
        || parts.at(1).toLower() != QLatin1String("to")) {
        return QStringLiteral("err config save takes live to <mode>");
    }
    const QString id = parts.at(2);

    if (!m_live)
        return QStringLiteral("err there is no live tuning to save");
    const Mode *existing = m_modes.find(id);
    if (!existing)
        return QStringLiteral("err unknown mode %1").arg(id);

    Mode edited = *existing;
    edited.setpoints = *m_live;
    QStringList clamped;
    normalizeMode(edited, &clamped);

    ModeTable modes = m_modes;
    modes.replace(edited);
    const QString reply = commitTables(modes, m_brightnessSettings, clamped);
    // Dropped only when it has become the mode in force: the applied values do
    // not change, and the status stops claiming an experiment is running when
    // what is running is now a saved definition. Saving into another mode
    // leaves the overlay alone, because dropping it there would move the
    // machine.
    if (reply.startsWith(QLatin1String("ok")) && id == m_state.mode)
        m_live.reset();
    return reply;
}

QString Daemon::configResetCommand(const QStringList &parts)
{
    if (parts.isEmpty())
        return QStringLiteral("err config reset needs modes, brightness or all");
    const QString what = parts.first().toLower();

    ModeTable modes = m_modes;
    BrightnessSettings brightness = m_brightnessSettings;
    if (what == QLatin1String("modes") || what == QLatin1String("all"))
        modes = ModeTable::builtinDefaults();
    else if (what != QLatin1String("brightness"))
        return QStringLiteral("err config reset takes modes, brightness or all");
    if (what == QLatin1String("brightness") || what == QLatin1String("all"))
        brightness = builtinBrightnessSettings();

    // A reset is the answer to "I have made a mess and want out", so it drops
    // the experiment too: an overlay surviving it would leave the machine on
    // numbers that are no longer written anywhere.
    if (what == QLatin1String("modes") || what == QLatin1String("all"))
        m_live.reset();

    return commitTables(modes, brightness, {});
}

QString Daemon::applyConfigCommand(const QStringList &parts)
{
    if (parts.isEmpty())
        return QStringLiteral("err config needs show, set, save, default or reset");
    const QString what = parts.first().toLower();

    if (what == QLatin1String("show"))
        return QStringLiteral("ok ") + configJson();

    if (what == QLatin1String("set")) {
        if (parts.size() < 2)
            return QStringLiteral("err config set needs mode or brightness");
        const QString kind = parts.at(1).toLower();
        if (kind == QLatin1String("mode"))
            return configModeCommand(parts.mid(2));
        if (kind == QLatin1String("brightness"))
            return configBrightnessCommand(parts.mid(2));
        return QStringLiteral("err config set takes mode or brightness");
    }

    if (what == QLatin1String("save"))
        return configSaveCommand(parts.mid(1));

    if (what == QLatin1String("reset"))
        return configResetCommand(parts.mid(1));

    if (what == QLatin1String("default_mode")) {
        if (parts.size() < 2)
            return QStringLiteral("err config default_mode needs a mode name");
        ModeTable modes = m_modes;
        if (!modes.setDefaultModeId(parts.at(1)))
            return QStringLiteral("err unknown mode %1").arg(parts.at(1));
        return commitTables(modes, m_brightnessSettings, {});
    }

    return QStringLiteral("err config takes show, set, save, default_mode or reset");
}

QString Daemon::statusJson() const
{
    const Setpoints requested = requestedSetpoints();
    const Sample &s = m_lastSample;

    QJsonObject applied;
    applied[QStringLiteral("profile")] = profileToString(m_lastApplied.profile);
    applied[QStringLiteral("pl1_w")] = m_lastApplied.pl1W;
    applied[QStringLiteral("pl2_w")] = m_lastApplied.pl2W;
    applied[QStringLiteral("max_perf_pct")] = m_lastApplied.maxPerfPct;
    applied[QStringLiteral("turbo")] = !m_lastApplied.noTurbo;
    applied[QStringLiteral("epp")] = eppToString(m_lastApplied.epp);
    applied[QStringLiteral("hwp_boost")] = m_lastApplied.hwpBoost;
    applied[QStringLiteral("gt_max_mhz")] = m_lastApplied.gtMaxMhz;
    applied[QStringLiteral("fan_mode")] = m_lastApplied.fanMode;

    QJsonObject wanted;
    wanted[QStringLiteral("profile")] = profileToString(requested.profile);
    wanted[QStringLiteral("pl1_w")] = requested.pl1W;
    wanted[QStringLiteral("pl2_w")] = requested.pl2W;
    wanted[QStringLiteral("max_perf_pct")] = requested.maxPerfPct;
    wanted[QStringLiteral("turbo")] = !requested.noTurbo;
    wanted[QStringLiteral("epp")] = eppToString(requested.epp);
    wanted[QStringLiteral("hwp_boost")] = requested.hwpBoost;
    wanted[QStringLiteral("gt_max_mhz")] = requested.gtMaxMhz;
    wanted[QStringLiteral("fan_mode")] = requested.fanMode;

    QJsonObject sensors;
    sensors[QStringLiteral("pkg_c")] = temperature(s.pkgTemp);
    sensors[QStringLiteral("board_c")] = temperature(s.boardTemp);
    sensors[QStringLiteral("nvme_c")] = temperature(s.nvmeTemp);
    sensors[QStringLiteral("fan_rpm")] = plain(s.fanRpm);
    sensors[QStringLiteral("pkg_w")] = centiwatts(s.pkgPower);
    sensors[QStringLiteral("core_w")] = centiwatts(s.corePower);
    sensors[QStringLiteral("uncore_w")] = centiwatts(s.uncorePower);
    sensors[QStringLiteral("cpu_mhz")] = plain(s.cpuMhz);
    sensors[QStringLiteral("gpu_mhz")] = plain(s.gpuMhz);
    sensors[QStringLiteral("load1")] = s.load1 == kAbsentValue ? QJsonValue()
                                                              : QJsonValue(s.load1 / 100.0);
    sensors[QStringLiteral("displays")] =
            s.displays == kAbsentPercent ? QJsonValue() : QJsonValue(s.displays);

    QJsonArray modeList;
    for (const Mode &mode : m_modes.modes()) {
        QJsonObject entry;
        entry[QStringLiteral("id")] = mode.id;
        entry[QStringLiteral("label")] = mode.label;
        modeList.append(entry);
    }

    // Its own object, because the backlight is its own function: the thermal
    // mode does not appear here and nothing here appears above.
    QJsonObject brightness;
    brightness[QStringLiteral("available")] = m_backlight.available();
    brightness[QStringLiteral("auto")] = m_state.autoBrightness;
    brightness[QStringLiteral("level")] =
            s.brightness == kAbsentPercent ? QJsonValue() : QJsonValue(s.brightness);
    brightness[QStringLiteral("base")] =
            s.brightnessBase == kAbsentPercent ? QJsonValue() : QJsonValue(s.brightnessBase);
    // Lux on the wire as well: the console shows nothing of the sensor, and a
    // diagnostic reading in millilux is a number nobody can picture.
    brightness[QStringLiteral("light_lux")] =
            s.light == kAbsentLight ? QJsonValue() : QJsonValue(s.light / 1000.0);
    // The two the console puts on sliders, and the band they produce. The band
    // is what the window actually shows, because "between 50 and 100 per cent"
    // is a sentence and "a response of thirty" is not.
    brightness[QStringLiteral("response_pct")] = m_brightnessSettings.responsePct;
    brightness[QStringLiteral("floor_pct")] = m_brightnessSettings.floorPct;
    if (m_state.brightnessBase != DaemonState::kBrightnessBaseUnset) {
        const BrightnessWindow window =
                brightnessWindow(m_brightnessSettings, m_state.brightnessBase);
        brightness[QStringLiteral("window_low")] = window.low;
        brightness[QStringLiteral("window_high")] = window.high;
    }

    QJsonObject root;
    root[QStringLiteral("ts")] = static_cast<qint64>(s.ts);
    root[QStringLiteral("mode")] = m_state.mode;
    root[QStringLiteral("modes")] = modeList;
    root[QStringLiteral("live_tuning")] = m_live.has_value();
    root[QStringLiteral("guard_engaged")] = m_guard.engaged;
    root[QStringLiteral("fan_forced")] = m_guard.fanForced;
    root[QStringLiteral("write_mismatch")] = (s.flags & FlagWriteMismatch) != 0;
    root[QStringLiteral("sensors_degraded")] = (s.flags & FlagSensorsDegraded) != 0;
    root[QStringLiteral("on_ac")] = (s.flags & FlagOnAc) != 0;
    root[QStringLiteral("charge_limit")] = s.chargeLimit == 0 ? QJsonValue()
                                                              : QJsonValue(s.chargeLimit);
    root[QStringLiteral("ticks")] = static_cast<qint64>(m_ticks);
    root[QStringLiteral("applied")] = applied;
    root[QStringLiteral("requested")] = wanted;
    root[QStringLiteral("sensors")] = sensors;
    root[QStringLiteral("brightness")] = brightness;

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QString Daemon::handleCommand(const QString &line)
{
    const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return QStringLiteral("err empty command");
    const QString verb = parts.first().toLower();

    if (verb == QLatin1String("status"))
        return QStringLiteral("ok ") + statusJson();

    if (verb == QLatin1String("mode")) {
        if (parts.size() < 2)
            return QStringLiteral("err mode needs a name");
        return applyMode(parts.at(1));
    }

    if (verb == QLatin1String("set")) {
        if (parts.size() < 2)
            return QStringLiteral("err set needs at least one assignment");
        return applyLiveSetting(parts.mid(1));
    }

    if (verb == QLatin1String("reset")) {
        m_live.reset();
        return QStringLiteral("ok %1").arg(m_state.mode);
    }

    if (verb == QLatin1String("brightness"))
        return applyBrightnessCommand(parts.mid(1));

    if (verb == QLatin1String("mark")) {
        if (parts.size() < 2)
            return QStringLiteral("err mark needs a verdict");
        const Mark mark = markFromString(parts.at(1));
        if (mark == Mark::None)
            return QStringLiteral("err unknown verdict %1").arg(parts.at(1));
        m_pendingMark = mark;
        // Stamp the sample already on disk as well: the judgement is about
        // what the machine was doing a moment ago, not about the next tick.
        m_ring.markNewest(mark);
        return QStringLiteral("ok");
    }

    if (verb == QLatin1String("fan")) {
        if (parts.size() < 2)
            return QStringLiteral("err fan needs max or auto");
        // A shorthand for the setpoint of the same name, and nothing more. The
        // fan is a setpoint like the power limits, so one mechanism carries it
        // and "full speed, right now" is one short command.
        return applyLiveSetting({ QStringLiteral("fan=") + parts.at(1).toLower() });
    }

    if (verb == QLatin1String("charge")) {
        if (parts.size() < 2)
            return QStringLiteral("err charge needs a percentage");
        bool ok = false;
        const int percent = parts.at(1).toInt(&ok);
        if (!ok)
            return QStringLiteral("err charge percentage is not a number");
        if (!m_powerSource.setChargeLimit(percent))
            return QStringLiteral("err charge control is not available");
        // Remembered, because the threshold lives in sysfs and a reboot loses
        // it. Read back rather than stored as typed: the node clamps.
        if (const auto applied = m_powerSource.chargeLimit()) {
            m_state.chargeLimit = *applied;
            saveState();
        }
        return QStringLiteral("ok");
    }

    if (verb == QLatin1String("config"))
        return applyConfigCommand(parts.mid(1));

    if (verb == QLatin1String("reload")) {
        loadConfiguration();
        return QStringLiteral("ok");
    }

    return QStringLiteral("err unknown command %1").arg(verb);
}

void Daemon::tick()
{
    const uint32_t now = static_cast<uint32_t>(QDateTime::currentSecsSinceEpoch());
    Sample sample = m_sensors.read(now, m_monotonic.elapsed());

    Setpoints requested = requestedSetpoints();
    if (sample.pkgTemp == kAbsentTemp) {
        // Without a package temperature there is no way to tell whether the
        // current setpoints are safe, and the emergency guard has nothing to
        // act on. The fallback is the most conservative mode, not the last
        // known one, and it overrides live tuning as well: an experiment does
        // not outrank the interlock it was relying on.
        if (const Mode *conservative = m_modes.mostConservative()) {
            // The fan is the exception. With no temperature reading more
            // airflow is never the wrong answer, so a fan already running flat
            // out keeps running.
            const int commandedFan = requested.fanMode;
            requested = conservative->setpoints;
            requested.fanMode = std::min(requested.fanMode, commandedFan);
        }
    }

    const Setpoints effective = applyGuard(sample, requested, m_guard);
    m_actuator.apply(effective);
    m_lastApplied = effective;

    // A separate path, run after the thermal one and sharing nothing with it.
    // The backlight is not part of the thermal envelope, and selecting a mode
    // must leave it exactly where it was.
    updateBrightness(sample);

    // A node or a sensor that was resolved and has since disappeared means the
    // device re-registered under a different hwmon index. Re-resolving is the
    // difference between a transient absence and one that lasts the rest of
    // the boot — and a guard that cannot see the package temperature never
    // releases.
    if (m_actuator.nodesVanished())
        m_actuator.resolve();
    if (sample.pkgTemp == kAbsentTemp)
        m_sensors.resolve();

    const int modeIndex = m_modes.indexOf(m_state.mode);
    sample.modeId = (modeIndex >= 0 && modeIndex < 255) ? static_cast<uint8_t>(modeIndex) : 0xFF;
    sample.pl1W = static_cast<uint8_t>(std::clamp(effective.pl1W, 0, 255));
    sample.pl2W = static_cast<uint8_t>(std::clamp(effective.pl2W, 0, 255));
    sample.maxPerfPct = static_cast<uint8_t>(std::clamp(effective.maxPerfPct, 0, 100));
    sample.noTurbo = effective.noTurbo ? 1 : 0;
    sample.epp = static_cast<uint8_t>(effective.epp);
    sample.hwpBoost = effective.hwpBoost ? 1 : 0;
    sample.profile = static_cast<uint8_t>(effective.profile);
    sample.gtMaxMhz = static_cast<uint16_t>(std::clamp(effective.gtMaxMhz, 0, 65535));
    sample.fanMode = static_cast<uint8_t>(effective.fanMode);
    if (const auto limit = m_powerSource.chargeLimit())
        sample.chargeLimit = static_cast<uint8_t>(std::clamp(*limit, 0, 255));
    if (m_powerSource.onAc())
        sample.flags |= FlagOnAc;
    if (m_guard.engaged)
        sample.flags |= FlagGuardEngaged;
    if (m_actuator.lastApplyMismatched())
        sample.flags |= FlagWriteMismatch;
    if (m_live)
        sample.flags |= FlagLiveTuning;
    sample.mark = static_cast<uint8_t>(m_pendingMark);
    m_pendingMark = Mark::None;

    m_ring.append(sample);
    m_lastSample = sample;
    ++m_ticks;

    if (++m_sinceFlush >= m_options.flushSeconds) {
        m_sinceFlush = 0;
        m_ring.flush();
    }

    if (m_options.maxTicks > 0 && m_ticks >= static_cast<quint64>(m_options.maxTicks))
        shutdown();
}
