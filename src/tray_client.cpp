// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "tray_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>

namespace {

// A single-line exchange over a local socket either completes at once or the
// daemon is not there. The timeouts are what keeps a missing daemon from
// stalling the interface rather than an allowance for a slow one.
constexpr int kConnectTimeoutMs = 1000;
constexpr int kReplyTimeoutMs = 1000;

// A reading the daemon could not take is JSON null, which is not zero: zero is
// a legal fan speed and a legal power draw.
double reading(const QJsonObject &sensors, QLatin1String key)
{
    const QJsonValue value = sensors.value(key);
    return value.isDouble() ? value.toDouble() : TrayClient::kNoReading;
}

Setpoints setpointsFrom(const QJsonObject &object)
{
    Setpoints points;
    const Profile profile = profileFromString(object.value(QLatin1String("profile")).toString());
    if (profile != Profile::Unknown)
        points.profile = profile;
    const Epp epp = eppFromString(object.value(QLatin1String("epp")).toString());
    if (epp != Epp::Unknown)
        points.epp = epp;
    points.pl1W = object.value(QLatin1String("pl1_w")).toInt(points.pl1W);
    points.pl2W = object.value(QLatin1String("pl2_w")).toInt(points.pl2W);
    points.maxPerfPct = object.value(QLatin1String("max_perf_pct")).toInt(points.maxPerfPct);
    points.noTurbo = !object.value(QLatin1String("turbo")).toBool(!points.noTurbo);
    points.hwpBoost = object.value(QLatin1String("hwp_boost")).toBool(points.hwpBoost);
    points.gtMaxMhz = object.value(QLatin1String("gt_max_mhz")).toInt(points.gtMaxMhz);
    // Only the applied setpoints carry a fan mode. A mode cannot ask for full
    // speed, so the requested object has no such field and keeps the default.
    points.fanMode = object.value(QLatin1String("fan_mode")).toInt(points.fanMode);
    return points;
}

} // namespace

TrayClient::TrayClient(QString socketPath, QObject *parent)
    : QObject(parent)
    , m_socketPath(std::move(socketPath))
{
    connect(&m_timer, &QTimer::timeout, this, &TrayClient::poll);
}

void TrayClient::start(int intervalMs)
{
    m_timer.setInterval(intervalMs);
    m_timer.start();
    poll();
}

void TrayClient::stop()
{
    m_timer.stop();
}

QString TrayClient::send(const QString &command)
{
    // One connection per exchange. The daemon may restart under a running
    // tray, and a socket that is opened for the command and closed after it
    // cannot be left holding a dead connection.
    QLocalSocket socket;
    socket.connectToServer(m_socketPath);
    if (!socket.waitForConnected(kConnectTimeoutMs))
        return {};

    socket.write(command.toUtf8());
    socket.write("\n");
    socket.flush();

    while (!socket.canReadLine()) {
        if (!socket.waitForReadyRead(kReplyTimeoutMs))
            return {};
    }
    return QString::fromUtf8(socket.readLine()).trimmed();
}

QString TrayClient::sendAndRefresh(const QString &command)
{
    const QString reply = send(command);
    poll();
    return reply;
}

void TrayClient::poll()
{
    const QString reply = send(QStringLiteral("status"));
    const QJsonDocument document = reply.startsWith(QLatin1String("ok "))
            ? QJsonDocument::fromJson(reply.mid(3).toUtf8())
            : QJsonDocument();
    if (!document.isObject()) {
        setConnected(false);
        return;
    }

    m_status = parseStatus(document.object());
    setConnected(true);
    emit statusChanged(m_status);
}

TrayClient::Configuration TrayClient::fetchConfiguration()
{
    const QString reply = send(QStringLiteral("config show"));
    if (!reply.startsWith(QLatin1String("ok ")))
        return {};
    return parseConfiguration(QJsonDocument::fromJson(reply.mid(3).toUtf8()).object());
}

TrayClient::Configuration TrayClient::parseConfiguration(const QJsonObject &object)
{
    Configuration configuration;
    configuration.valid = object.contains(QLatin1String("modes"));
    configuration.path = object.value(QLatin1String("path")).toString();
    configuration.stored = object.value(QLatin1String("stored")).toBool();
    configuration.defaultModeId = object.value(QLatin1String("default_mode")).toString();

    for (const QJsonValue &entry : object.value(QLatin1String("modes")).toArray()) {
        const QJsonObject mode = entry.toObject();
        ModeDefinition definition;
        definition.id = mode.value(QLatin1String("id")).toString();
        definition.label = mode.value(QLatin1String("label")).toString(definition.id);
        // The saved side speaks the settings vocabulary — `pl1_w`, not the
        // `pl1` of live tuning — because that is the file's own language.
        definition.setpoints.profile =
                profileFromString(mode.value(QLatin1String("platform_profile")).toString());
        definition.setpoints.pl1W = mode.value(QLatin1String("pl1_w")).toInt();
        definition.setpoints.pl2W = mode.value(QLatin1String("pl2_w")).toInt();
        definition.setpoints.maxPerfPct = mode.value(QLatin1String("max_perf_pct")).toInt();
        definition.setpoints.noTurbo = !mode.value(QLatin1String("turbo")).toBool();
        definition.setpoints.epp = eppFromString(mode.value(QLatin1String("epp")).toString());
        definition.setpoints.hwpBoost = mode.value(QLatin1String("hwp_dynamic_boost")).toBool();
        definition.setpoints.gtMaxMhz = mode.value(QLatin1String("gt_max_mhz")).toInt();
        configuration.modes.append(definition);
    }
    return configuration;
}

TrayClient::Status TrayClient::parseStatus(const QJsonObject &object)
{
    Status status;
    status.valid = object.contains(QLatin1String("mode"));
    status.modeId = object.value(QLatin1String("mode")).toString();

    const QJsonArray modes = object.value(QLatin1String("modes")).toArray();
    for (const QJsonValue &entry : modes) {
        const QJsonObject mode = entry.toObject();
        const QString id = mode.value(QLatin1String("id")).toString();
        status.modeIds.append(id);
        status.modeLabels.append(mode.value(QLatin1String("label")).toString(id));
    }

    status.liveTuning = object.value(QLatin1String("live_tuning")).toBool();
    status.guardEngaged = object.value(QLatin1String("guard_engaged")).toBool();
    status.fanForced = object.value(QLatin1String("fan_forced")).toBool();
    status.writeMismatch = object.value(QLatin1String("write_mismatch")).toBool();
    status.sensorsDegraded = object.value(QLatin1String("sensors_degraded")).toBool();
    status.onAc = object.value(QLatin1String("on_ac")).toBool();

    status.applied = setpointsFrom(object.value(QLatin1String("applied")).toObject());
    status.requested = setpointsFrom(object.value(QLatin1String("requested")).toObject());

    const QJsonObject sensors = object.value(QLatin1String("sensors")).toObject();
    status.packageC = reading(sensors, QLatin1String("pkg_c"));
    status.boardC = reading(sensors, QLatin1String("board_c"));
    status.nvmeC = reading(sensors, QLatin1String("nvme_c"));
    status.fanRpm = reading(sensors, QLatin1String("fan_rpm"));
    status.packageW = reading(sensors, QLatin1String("pkg_w"));
    status.coreW = reading(sensors, QLatin1String("core_w"));
    status.uncoreW = reading(sensors, QLatin1String("uncore_w"));
    status.cpuMhz = reading(sensors, QLatin1String("cpu_mhz"));
    status.gpuMhz = reading(sensors, QLatin1String("gpu_mhz"));
    status.load1 = reading(sensors, QLatin1String("load1"));

    const QJsonObject brightness = object.value(QLatin1String("brightness")).toObject();
    status.brightnessAvailable = brightness.value(QLatin1String("available")).toBool();
    status.autoBrightness = brightness.value(QLatin1String("auto")).toBool();
    status.brightness = reading(brightness, QLatin1String("level"));
    status.brightnessBase = reading(brightness, QLatin1String("base"));
    // The sensor reading is deliberately not carried into the window. What the
    // room measures is the daemon's business; the person is shown per cent.
    status.brightnessResponsePct = brightness.value(QLatin1String("response_pct"))
                                           .toInt(status.brightnessResponsePct);
    status.brightnessFloorPct = brightness.value(QLatin1String("floor_pct"))
                                        .toInt(status.brightnessFloorPct);
    status.brightnessCeilingPct = brightness.value(QLatin1String("ceiling_pct"))
                                          .toInt(status.brightnessCeilingPct);
    status.brightnessWindowLow = brightness.value(QLatin1String("window_low"))
                                         .toInt(status.brightnessFloorPct);
    status.brightnessWindowHigh = brightness.value(QLatin1String("window_high"))
                                          .toInt(status.brightnessCeilingPct);

    const QJsonObject metrics = object.value(QLatin1String("metrics")).toObject();
    status.retentionDays = metrics.value(QLatin1String("retention_days"))
                                   .toInt(status.retentionDays);
    status.metricsEnabled = metrics.value(QLatin1String("enabled")).toBool(status.metricsEnabled);
    return status;
}

void TrayClient::setConnected(bool connected)
{
    if (m_connected == connected)
        return;
    m_connected = connected;
    emit connectionChanged(connected);
}
