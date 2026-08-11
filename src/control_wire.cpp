// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "control_wire.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QList>

namespace {

// What the tick measures. A reading the machine could not take is JSON null, and
// null has no place in a dictionary of variants: the key is left out on the way to
// the bus and put back as null on the way to a client, which is what keeps a
// missing sensor distinguishable from a fan that is standing still.
constexpr const char *kSensorKeys[] = { "pkg_c",   "board_c",  "nvme_c",  "fan_rpm",
                                        "pkg_w",   "core_w",   "uncore_w", "cpu_mhz",
                                        "gpu_mhz", "load1",    "displays" };
// Conditions of the last tick rather than settings, so they travel with the
// readings: what the guard is doing is a fact about the sample.
constexpr const char *kFlagKeys[] = { "guard_engaged", "fan_forced", "write_mismatch",
                                      "sensors_degraded", "on_ac" };

// Absent, for a number that has to travel as one. The properties that carry a
// level, a baseline or a band use it, because a nought there would be a screen at
// nought per cent.
constexpr int kAbsent = -1;

void copyPresent(const QJsonObject &from, const char *key, QVariantMap &to)
{
    const QJsonValue value = from.value(QLatin1String(key));
    if (!value.isNull() && !value.isUndefined())
        to.insert(QString::fromLatin1(key), value.toVariant());
}

QJsonValue orNull(const QVariantMap &from, const char *key)
{
    const auto found = from.constFind(QString::fromLatin1(key));
    return found == from.constEnd() ? QJsonValue() : QJsonValue::fromVariant(*found);
}

int number(const QVariantMap &from, const char *key, int fallback = 0)
{
    const auto found = from.constFind(QString::fromLatin1(key));
    return found == from.constEnd() ? fallback : found->toInt();
}

QList<QVariantMap> mapsFrom(const QJsonArray &array)
{
    QList<QVariantMap> list;
    for (const QJsonValue &entry : array)
        list.append(entry.toObject().toVariantMap());
    return list;
}

QJsonArray arrayFrom(const QVariant &value)
{
    QJsonArray array;
    for (const QVariantMap &entry : value.value<QList<QVariantMap>>())
        array.append(QJsonObject::fromVariantMap(entry));
    return array;
}

// A monitor that stopped answering has no level, and the level it last had is not
// it: switched off, or its input moved to another machine. On the bus that is the
// absent number rather than a missing key, because every entry has to have the
// same shape as its neighbours.
QList<QVariantMap> monitorsFrom(const QJsonArray &array)
{
    QList<QVariantMap> list;
    for (const QJsonValue &value : array) {
        const QJsonObject entry = value.toObject();
        QVariantMap monitor = entry.toVariantMap();
        monitor.insert(QStringLiteral("level"),
                       entry.value(QLatin1String("level")).toInt(kAbsent));
        list.append(monitor);
    }
    return list;
}

QJsonArray monitorsJsonFrom(const QVariant &value)
{
    QJsonArray array;
    for (const QVariantMap &monitor : value.value<QList<QVariantMap>>()) {
        QJsonObject entry = QJsonObject::fromVariantMap(monitor);
        if (!monitor.value(QStringLiteral("answering")).toBool())
            entry.insert(QStringLiteral("level"), QJsonValue());
        array.append(entry);
    }
    return array;
}

} // namespace

namespace wire {

QVariantMap propertiesFrom(const QJsonObject &status, const QJsonObject &configuration)
{
    const QJsonObject sensors = status.value(QLatin1String("sensors")).toObject();
    const QJsonObject brightness = status.value(QLatin1String("brightness")).toObject();
    const QJsonObject metrics = status.value(QLatin1String("metrics")).toObject();

    QVariantMap readings;
    readings.insert(QStringLiteral("ts"), status.value(QLatin1String("ts")).toVariant());
    readings.insert(QStringLiteral("ticks"), status.value(QLatin1String("ticks")).toVariant());
    for (const char *key : kFlagKeys)
        readings.insert(QString::fromLatin1(key), status.value(QLatin1String(key)).toBool());
    for (const char *key : kSensorKeys)
        copyPresent(sensors, key, readings);
    // The light the room has, which belongs to the tick like every other reading.
    // What is done with it — the baseline, the band — does not.
    copyPresent(brightness, "light_lux", readings);

    QVariantMap properties;
    properties.insert(QString::fromLatin1(kReadings), readings);
    properties.insert(QStringLiteral("Mode"), status.value(QLatin1String("mode")).toString());
    properties.insert(QStringLiteral("Modes"),
                      QVariant::fromValue(mapsFrom(status.value(QLatin1String("modes")).toArray())));
    properties.insert(QStringLiteral("LiveTuning"),
                      status.value(QLatin1String("live_tuning")).toBool());
    properties.insert(QStringLiteral("Requested"),
                      status.value(QLatin1String("requested")).toObject().toVariantMap());
    properties.insert(QStringLiteral("Applied"),
                      status.value(QLatin1String("applied")).toObject().toVariantMap());
    // Nought is how the daemon says there is no threshold, and the JSON says it with
    // null. Both mean the battery charges to the top.
    properties.insert(QStringLiteral("ChargeLimit"),
                      status.value(QLatin1String("charge_limit")).toInt(0));

    properties.insert(QStringLiteral("BrightnessAvailable"),
                      brightness.value(QLatin1String("available")).toBool());
    properties.insert(QStringLiteral("AutoBrightness"),
                      brightness.value(QLatin1String("auto")).toBool());
    properties.insert(QStringLiteral("BrightnessLevel"),
                      brightness.value(QLatin1String("level")).toInt(kAbsent));
    properties.insert(QStringLiteral("BrightnessBase"),
                      brightness.value(QLatin1String("base")).toInt(kAbsent));
    properties.insert(QStringLiteral("BrightnessResponsePct"),
                      brightness.value(QLatin1String("response_pct")).toInt());
    properties.insert(QStringLiteral("BrightnessFloorPct"),
                      brightness.value(QLatin1String("floor_pct")).toInt());
    properties.insert(QStringLiteral("BrightnessCeilingPct"),
                      brightness.value(QLatin1String("ceiling_pct")).toInt());
    // The band exists only once there is a baseline to build it around.
    properties.insert(QStringLiteral("BrightnessWindowLow"),
                      brightness.value(QLatin1String("window_low")).toInt(kAbsent));
    properties.insert(QStringLiteral("BrightnessWindowHigh"),
                      brightness.value(QLatin1String("window_high")).toInt(kAbsent));

    properties.insert(QStringLiteral("Monitors"),
                      QVariant::fromValue(
                              monitorsFrom(status.value(QLatin1String("monitors")).toArray())));

    properties.insert(QStringLiteral("MetricsEnabled"),
                      metrics.value(QLatin1String("enabled")).toBool());
    properties.insert(QStringLiteral("RetentionDays"),
                      metrics.value(QLatin1String("retention_days")).toInt());
    properties.insert(QStringLiteral("SampleCount"),
                      static_cast<qulonglong>(metrics.value(QLatin1String("samples"))
                                                      .toInteger()));

    // What the daemon has saved, as opposed to what it is running. A property rather
    // than a question asked once, so that a definition edited from the command line
    // reaches an open console without it having to ask again.
    properties.insert(QStringLiteral("Configuration"), configuration.toVariantMap());
    return properties;
}

QJsonObject statusJsonFrom(const QVariantMap &properties)
{
    const QVariantMap readings = properties.value(QString::fromLatin1(kReadings)).toMap();

    QJsonObject sensors;
    for (const char *key : kSensorKeys)
        sensors.insert(QString::fromLatin1(key), orNull(readings, key));

    QJsonObject brightness;
    brightness.insert(QStringLiteral("available"),
                      properties.value(QStringLiteral("BrightnessAvailable")).toBool());
    brightness.insert(QStringLiteral("auto"),
                      properties.value(QStringLiteral("AutoBrightness")).toBool());
    const int level = number(properties, "BrightnessLevel", kAbsent);
    brightness.insert(QStringLiteral("level"), level == kAbsent ? QJsonValue() : QJsonValue(level));
    const int base = number(properties, "BrightnessBase", kAbsent);
    brightness.insert(QStringLiteral("base"), base == kAbsent ? QJsonValue() : QJsonValue(base));
    brightness.insert(QStringLiteral("light_lux"), orNull(readings, "light_lux"));
    brightness.insert(QStringLiteral("response_pct"),
                      number(properties, "BrightnessResponsePct"));
    brightness.insert(QStringLiteral("floor_pct"), number(properties, "BrightnessFloorPct"));
    brightness.insert(QStringLiteral("ceiling_pct"), number(properties, "BrightnessCeilingPct"));
    const int windowLow = number(properties, "BrightnessWindowLow", kAbsent);
    const int windowHigh = number(properties, "BrightnessWindowHigh", kAbsent);
    if (windowLow != kAbsent && windowHigh != kAbsent) {
        brightness.insert(QStringLiteral("window_low"), windowLow);
        brightness.insert(QStringLiteral("window_high"), windowHigh);
    }

    QJsonObject metrics;
    metrics.insert(QStringLiteral("retention_days"), number(properties, "RetentionDays"));
    metrics.insert(QStringLiteral("enabled"),
                   properties.value(QStringLiteral("MetricsEnabled")).toBool());
    metrics.insert(QStringLiteral("samples"),
                   static_cast<qint64>(properties.value(QStringLiteral("SampleCount"))
                                               .toULongLong()));

    QJsonObject root;
    root.insert(QStringLiteral("ts"), orNull(readings, "ts"));
    root.insert(QStringLiteral("mode"), properties.value(QStringLiteral("Mode")).toString());
    root.insert(QStringLiteral("modes"), arrayFrom(properties.value(QStringLiteral("Modes"))));
    root.insert(QStringLiteral("live_tuning"),
                properties.value(QStringLiteral("LiveTuning")).toBool());
    for (const char *key : kFlagKeys)
        root.insert(QString::fromLatin1(key), readings.value(QString::fromLatin1(key)).toBool());
    const int chargeLimit = number(properties, "ChargeLimit");
    root.insert(QStringLiteral("charge_limit"),
                chargeLimit == 0 ? QJsonValue() : QJsonValue(chargeLimit));
    root.insert(QStringLiteral("ticks"), orNull(readings, "ticks"));
    root.insert(QStringLiteral("applied"),
                QJsonObject::fromVariantMap(properties.value(QStringLiteral("Applied")).toMap()));
    root.insert(QStringLiteral("requested"),
                QJsonObject::fromVariantMap(properties.value(QStringLiteral("Requested")).toMap()));
    root.insert(QStringLiteral("sensors"), sensors);
    root.insert(QStringLiteral("brightness"), brightness);
    root.insert(QStringLiteral("monitors"),
                monitorsJsonFrom(properties.value(QStringLiteral("Monitors"))));
    root.insert(QStringLiteral("metrics"), metrics);
    return root;
}

} // namespace wire

namespace {

// One assignment of the command vocabulary, as a key and a value. Values arrive as
// variants because a caller on the bus may say 30 where the command line can only
// say "30", and the daemon's parser takes the text of either.
QString assignment(const QString &key, const QVariant &value, QString *error)
{
    const QString text = value.toString();
    // The daemon's parser splits a command on spaces, so a value cannot contain
    // one. Only the curve is long enough for anybody to try: it is written
    // "10:20,100:50", commas and no spaces.
    if (text.contains(QLatin1Char(' '))) {
        if (error)
            *error = QStringLiteral("the value of %1 cannot contain a space").arg(key);
        return {};
    }
    return key + QLatin1Char('=') + text;
}

QStringList assignments(const QVariantMap &values, QString *error)
{
    QStringList list;
    for (auto entry = values.constBegin(); entry != values.constEnd(); ++entry) {
        const QString one = assignment(entry.key(), entry.value(), error);
        if (one.isEmpty())
            return {};
        list.append(one);
    }
    return list;
}

QVariantMap valuesFrom(const QStringList &words, QString *error)
{
    QVariantMap values;
    for (const QString &word : words) {
        const int equals = word.indexOf(QLatin1Char('='));
        if (equals <= 0) {
            if (error)
                *error = QStringLiteral("malformed assignment %1").arg(word);
            return {};
        }
        values.insert(word.left(equals), word.mid(equals + 1));
    }
    return values;
}

wire::Call refused(const QString &reason)
{
    wire::Call call;
    call.error = reason;
    return call;
}

} // namespace

namespace wire {

Call callFor(const QStringList &words)
{
    if (words.isEmpty())
        return refused(QStringLiteral("no command given"));

    const QString verb = words.first().toLower();
    const QStringList rest = words.mid(1);
    Call call;

    if (verb == QLatin1String("status")) {
        call.read = Call::Read::Status;
        return call;
    }
    if (verb == QLatin1String("mode")) {
        if (rest.isEmpty())
            return refused(QStringLiteral("mode needs a name"));
        call.method = QStringLiteral("SetMode");
        call.arguments = { rest.first() };
        return call;
    }
    if (verb == QLatin1String("set")) {
        if (rest.isEmpty())
            return refused(QStringLiteral("set needs at least one assignment"));
        QString error;
        const QVariantMap values = valuesFrom(rest, &error);
        if (values.isEmpty())
            return refused(error);
        call.method = QStringLiteral("SetLive");
        call.arguments = { values };
        return call;
    }
    if (verb == QLatin1String("reset")) {
        call.method = QStringLiteral("ResetLive");
        return call;
    }
    // The fan is a setpoint like the power limits, and "full speed, right now" is
    // one short word for the same live change.
    if (verb == QLatin1String("fan")) {
        if (rest.isEmpty())
            return refused(QStringLiteral("fan needs max or auto"));
        call.method = QStringLiteral("SetLive");
        call.arguments = { QVariantMap { { QStringLiteral("fan"), rest.first().toLower() } } };
        return call;
    }
    if (verb == QLatin1String("mark")) {
        if (rest.isEmpty())
            return refused(QStringLiteral("mark needs a verdict"));
        call.method = QStringLiteral("Mark");
        call.arguments = { rest.first() };
        return call;
    }
    if (verb == QLatin1String("charge")) {
        if (rest.isEmpty())
            return refused(QStringLiteral("charge needs a percentage"));
        call.method = QStringLiteral("SetChargeLimit");
        call.arguments = { rest.first().toInt() };
        return call;
    }
    if (verb == QLatin1String("reload")) {
        call.method = QStringLiteral("Reload");
        return call;
    }

    if (verb == QLatin1String("brightness")) {
        if (rest.isEmpty())
            return refused(QStringLiteral("brightness takes on, off, level, base or set"));
        const QString what = rest.first().toLower();
        if (what == QLatin1String("on") || what == QLatin1String("off")) {
            call.method = QStringLiteral("SetAutoBrightness");
            call.arguments = { what == QLatin1String("on") };
            return call;
        }
        if (what == QLatin1String("level") || what == QLatin1String("base")) {
            if (rest.size() < 2)
                return refused(QStringLiteral("brightness %1 needs a percentage").arg(what));
            call.method = what == QLatin1String("level")
                    ? QStringLiteral("SetBrightnessLevel")
                    : QStringLiteral("SetBrightnessBase");
            call.arguments = { rest.at(1).toInt() };
            return call;
        }
        if (what == QLatin1String("set")) {
            QString error;
            const QVariantMap values = valuesFrom(rest.mid(1), &error);
            if (values.isEmpty())
                return refused(error.isEmpty() ? QStringLiteral("brightness set needs key=value")
                                               : error);
            call.method = QStringLiteral("SetBrightnessSettings");
            call.arguments = { values };
            return call;
        }
        return refused(QStringLiteral("brightness takes on, off, level, base or set"));
    }

    if (verb == QLatin1String("monitor")) {
        if (rest.isEmpty())
            return refused(QStringLiteral("monitor needs list or level"));
        const QString what = rest.first().toLower();
        if (what == QLatin1String("list")) {
            call.read = Call::Read::Monitors;
            return call;
        }
        if (what == QLatin1String("level")) {
            if (rest.size() < 2)
                return refused(QStringLiteral("monitor level needs a percentage"));
            // Without an identifier: every monitor that answers. One screen is the
            // common case and naming it every time would be ceremony.
            call.method = QStringLiteral("SetMonitorLevel");
            call.arguments = { rest.size() >= 3 ? rest.at(2) : QString(), rest.at(1).toInt() };
            return call;
        }
        return refused(QStringLiteral("monitor takes list or level"));
    }

    if (verb == QLatin1String("config")) {
        if (rest.isEmpty())
            return refused(QStringLiteral("config takes show, set, save, reset or default_mode"));
        const QString what = rest.first().toLower();
        if (what == QLatin1String("show")) {
            call.read = Call::Read::Configuration;
            return call;
        }
        if (what == QLatin1String("reload")) {
            call.method = QStringLiteral("Reload");
            return call;
        }
        if (what == QLatin1String("default_mode")) {
            if (rest.size() < 2)
                return refused(QStringLiteral("config default_mode needs a mode name"));
            call.method = QStringLiteral("SetDefaultMode");
            call.arguments = { rest.at(1) };
            return call;
        }
        if (what == QLatin1String("reset")) {
            if (rest.size() < 2)
                return refused(QStringLiteral("config reset needs modes, brightness, "
                                              "metrics or all"));
            call.method = QStringLiteral("ResetConfiguration");
            call.arguments = { rest.at(1).toLower() };
            return call;
        }
        if (what == QLatin1String("save")) {
            // "config save live to <id>", spelled out because keeping an experiment
            // is a decision with a destination.
            if (rest.size() != 4 || rest.at(1).toLower() != QLatin1String("live")
                || rest.at(2).toLower() != QLatin1String("to")) {
                return refused(QStringLiteral("config save takes: save live to <mode>"));
            }
            call.method = QStringLiteral("SaveLiveAs");
            call.arguments = { rest.at(3) };
            return call;
        }
        if (what == QLatin1String("set")) {
            if (rest.size() < 2)
                return refused(QStringLiteral("config set takes mode, brightness or metrics"));
            const QString kind = rest.at(1).toLower();
            QStringList words = rest.mid(2);
            QVariantMap values;
            // A mode is named before its assignments, and the name travels in the map
            // rather than as an argument of its own: one method for all three kinds.
            if (kind == QLatin1String("mode")) {
                if (words.isEmpty())
                    return refused(QStringLiteral("config set mode needs a mode name"));
                values.insert(QStringLiteral("id"), words.takeFirst());
            }
            QString error;
            const QVariantMap rest_ = valuesFrom(words, &error);
            if (rest_.isEmpty() && !words.isEmpty())
                return refused(error);
            for (auto entry = rest_.constBegin(); entry != rest_.constEnd(); ++entry)
                values.insert(entry.key(), entry.value());
            call.method = QStringLiteral("Configure");
            call.arguments = { kind, values };
            return call;
        }
        return refused(QStringLiteral("config takes show, set, save, reset or default_mode"));
    }

    return refused(QStringLiteral("unknown command %1").arg(verb));
}

QString commandFor(const QString &method, const QVariantList &arguments, QString *error)
{
    const auto text = [&arguments](int index) {
        return arguments.value(index).toString();
    };
    const auto number = [&arguments](int index) {
        return QString::number(arguments.value(index).toInt());
    };
    const auto fail = [error](const QString &reason) {
        if (error)
            *error = reason;
        return QString();
    };

    if (method == QLatin1String("SetMode"))
        return QStringLiteral("mode ") + text(0);
    if (method == QLatin1String("ResetLive"))
        return QStringLiteral("reset");
    if (method == QLatin1String("Reload"))
        return QStringLiteral("reload");
    if (method == QLatin1String("Mark"))
        return QStringLiteral("mark ") + text(0);
    if (method == QLatin1String("SetChargeLimit"))
        return QStringLiteral("charge ") + number(0);
    if (method == QLatin1String("SetAutoBrightness")) {
        return QStringLiteral("brightness ")
                + (arguments.value(0).toBool() ? QStringLiteral("on") : QStringLiteral("off"));
    }
    if (method == QLatin1String("SetBrightnessLevel"))
        return QStringLiteral("brightness level ") + number(0);
    if (method == QLatin1String("SetBrightnessBase"))
        return QStringLiteral("brightness base ") + number(0);
    if (method == QLatin1String("SetDefaultMode"))
        return QStringLiteral("config default_mode ") + text(0);
    if (method == QLatin1String("ResetConfiguration"))
        return QStringLiteral("config reset ") + text(0);
    if (method == QLatin1String("SaveLiveAs"))
        return QStringLiteral("config save live to ") + text(0);
    if (method == QLatin1String("SetMonitorLevel")) {
        const QString id = text(0);
        const QString command = QStringLiteral("monitor level ") + number(1);
        return id.isEmpty() ? command : command + QLatin1Char(' ') + id;
    }

    if (method == QLatin1String("SetLive") || method == QLatin1String("SetBrightnessSettings")) {
        QString reason;
        const QStringList list = assignments(arguments.value(0).toMap(), &reason);
        if (list.isEmpty())
            return fail(reason.isEmpty() ? QStringLiteral("no assignment given") : reason);
        const QString verb = method == QLatin1String("SetLive")
                ? QStringLiteral("set ")
                : QStringLiteral("brightness set ");
        return verb + list.join(QLatin1Char(' '));
    }

    if (method == QLatin1String("Configure")) {
        const QString kind = text(0);
        if (kind != QLatin1String("mode") && kind != QLatin1String("brightness")
            && kind != QLatin1String("metrics")) {
            return fail(QStringLiteral("config set takes mode, brightness or metrics"));
        }
        QVariantMap values = arguments.value(1).toMap();
        QString id;
        if (kind == QLatin1String("mode")) {
            id = values.take(QStringLiteral("id")).toString();
            if (id.isEmpty())
                return fail(QStringLiteral("config set mode needs a mode name"));
        }
        QString reason;
        const QStringList list = assignments(values, &reason);
        if (list.isEmpty())
            return fail(reason.isEmpty() ? QStringLiteral("no assignment given") : reason);
        QStringList command { QStringLiteral("config"), QStringLiteral("set"), kind };
        if (!id.isEmpty())
            command.append(id);
        command.append(list);
        return command.join(QLatin1Char(' '));
    }

    return fail(QStringLiteral("unknown method %1").arg(method));
}

} // namespace wire
