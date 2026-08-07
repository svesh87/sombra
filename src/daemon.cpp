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
    , m_brightnessProfiles(BrightnessProfileTable::builtinDefaults())
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
    ModeTable table;
    QStringList problems;
    if (table.load(m_options.configPath, &problems)) {
        m_modes = table;
    } else {
        m_modes = ModeTable::builtinDefaults();
    }

    // The same file, a separate list and a separate selection: the brightness
    // side shares the configuration file with the modes and nothing else.
    BrightnessProfileTable brightness;
    if (brightness.load(m_options.configPath, &problems)) {
        m_brightnessProfiles = brightness;
    } else {
        m_brightnessProfiles = BrightnessProfileTable::builtinDefaults();
    }

    for (const QString &problem : problems)
        qWarning().noquote() << "configuration:" << problem;

    if (!m_modes.find(m_state.mode))
        m_state.mode = m_modes.defaultModeId();
    if (!m_brightnessProfiles.find(m_state.brightnessProfile))
        m_state.brightnessProfile = m_brightnessProfiles.defaultProfileId();

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
    if (!m_brightnessProfiles.find(m_state.brightnessProfile))
        m_state.brightnessProfile = m_brightnessProfiles.defaultProfileId();

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
    if (m_fanOverride)
        points.fanMode = *m_fanOverride;
    return points;
}

QString Daemon::applyMode(const QString &id)
{
    const Mode *mode = m_modes.find(id);
    if (!mode)
        return QStringLiteral("err unknown mode %1").arg(id);
    // Selecting a mode is the deliberate act that cancels live tuning and any
    // fan command; the last command wins.
    m_live.reset();
    m_fanOverride.reset();
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
    // person on the brightness keys. Their level is the new base, and the
    // window moves with it. The step is dropped so the next decision only
    // adopts a position: an automatic move on the heels of a hand correction
    // is the automation arguing with the person.
    const int percent = m_backlight.toPercent(*current);
    stopBrightnessFade();
    m_brightness.step = BrightnessState::kUnset;
    m_brightness.pending = BrightnessState::kUnset;
    m_brightness.pendingTicks = 0;
    if (m_state.brightnessBase != percent) {
        m_state.brightnessBase = percent;
        saveState();
    }
}

void Daemon::updateBrightness(Sample &sample)
{
    sample.light = kAbsentValue;
    sample.brightness = kAbsentPercent;
    sample.brightnessBase = kAbsentPercent;
    sample.brightnessProfile = kAbsentPercent;

    if (!m_backlight.available())
        return;

    const int profileIndex = m_brightnessProfiles.indexOf(m_state.brightnessProfile);
    if (profileIndex >= 0 && profileIndex < kAbsentPercent)
        sample.brightnessProfile = static_cast<uint8_t>(profileIndex);

    // Read the panel before deciding anything: our own write would overwrite a
    // hand correction before it had been seen.
    noticeManualBrightness();

    if (const auto percent = m_backlight.readPercent())
        sample.brightness = static_cast<uint8_t>(*percent);
    if (m_state.brightnessBase != DaemonState::kBrightnessBaseUnset)
        sample.brightnessBase = static_cast<uint8_t>(m_state.brightnessBase);

    const auto light = m_lightSensor.read();
    if (light)
        sample.light = static_cast<uint16_t>(*light);

    // Switched off, the automation does not write the backlight at all. Not
    // "moves it less" — at all.
    if (!m_state.autoBrightness)
        return;
    sample.flags |= FlagAutoBrightness;

    const BrightnessProfile *profile = m_brightnessProfiles.find(m_state.brightnessProfile);
    if (!profile)
        return;

    m_brightness.base = m_state.brightnessBase;
    const BrightnessDecision decision = decideBrightness(*profile, light, m_brightness);
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

    if (what == QLatin1String("profile")) {
        if (parts.size() < 2)
            return QStringLiteral("err brightness profile needs a name");
        const QString id = parts.at(1);
        if (!m_brightnessProfiles.find(id))
            return QStringLiteral("err unknown brightness profile %1").arg(id);
        if (m_state.brightnessProfile != id) {
            m_state.brightnessProfile = id;
            saveState();
        }
        // A different window, adopted rather than jumped into.
        m_brightness.step = BrightnessState::kUnset;
        m_brightness.pending = BrightnessState::kUnset;
        m_brightness.pendingTicks = 0;
        return QStringLiteral("ok %1").arg(id);
    }

    return QStringLiteral("err brightness takes on, off or profile");
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
        } else {
            return QStringLiteral("err unknown setting %1").arg(key);
        }
    }
    // Live values apply immediately and are never written back anywhere. When
    // one is worth keeping, the tray offers it as a configuration snippet and
    // promoting it stays a deliberate edit.
    m_live = points;
    return QStringLiteral("ok");
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

    QJsonArray modeList;
    for (const Mode &mode : m_modes.modes()) {
        QJsonObject entry;
        entry[QStringLiteral("id")] = mode.id;
        entry[QStringLiteral("label")] = mode.label;
        modeList.append(entry);
    }

    QJsonArray brightnessProfileList;
    for (const BrightnessProfile &profile : m_brightnessProfiles.profiles()) {
        QJsonObject entry;
        entry[QStringLiteral("id")] = profile.id;
        entry[QStringLiteral("label")] = profile.label;
        brightnessProfileList.append(entry);
    }

    // Its own object, because the backlight is its own function: the thermal
    // mode does not appear here and nothing here appears above.
    QJsonObject brightness;
    brightness[QStringLiteral("available")] = m_backlight.available();
    brightness[QStringLiteral("auto")] = m_state.autoBrightness;
    brightness[QStringLiteral("profile")] = m_state.brightnessProfile;
    brightness[QStringLiteral("profiles")] = brightnessProfileList;
    brightness[QStringLiteral("level")] =
            s.brightness == kAbsentPercent ? QJsonValue() : QJsonValue(s.brightness);
    brightness[QStringLiteral("base")] =
            s.brightnessBase == kAbsentPercent ? QJsonValue() : QJsonValue(s.brightnessBase);
    brightness[QStringLiteral("light")] = plain(s.light);
    if (const BrightnessProfile *profile = m_brightnessProfiles.find(m_state.brightnessProfile);
        profile && m_state.brightnessBase != DaemonState::kBrightnessBaseUnset) {
        const BrightnessWindow window = brightnessWindow(*profile, m_state.brightnessBase);
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
        m_fanOverride.reset();
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
        const QString what = parts.at(1).toLower();
        if (what != QLatin1String("max") && what != QLatin1String("auto"))
            return QStringLiteral("err fan takes max or auto");
        // Full speed is reachable only by explicit command like this one;
        // automatic control never engages it. It is held apart from live
        // tuning so that a mode reload still reaches the other setpoints.
        m_fanOverride = what == QLatin1String("max") ? 0 : 2;
        return QStringLiteral("ok");
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

    if (verb == QLatin1String("reload")) {
        loadConfiguration();
        return QStringLiteral("ok");
    }

    if (verb == QLatin1String("snippet")) {
        const QString id = parts.size() > 1 ? parts.at(1) : m_state.mode;
        return QStringLiteral("ok ")
                + QString::fromUtf8(
                        ModeTable::toConfigSnippet(id, requestedSetpoints()).toUtf8().toBase64());
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
            // A fan command is the exception. Only an explicit request ever
            // sets this, and with no temperature reading more airflow is never
            // the wrong answer.
            const int commandedFan = requested.fanMode;
            requested = conservative->setpoints;
            requested.fanMode = commandedFan;
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
