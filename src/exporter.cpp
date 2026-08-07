// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "exporter.h"

#include "setpoints.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <optional>

namespace {

// One place for the absence rules, so the two output formats cannot drift
// apart on which readings are missing.
std::optional<double> temperature(int16_t deci)
{
    if (deci == kAbsentTemp)
        return std::nullopt;
    return deci / 10.0;
}

// `divisor` carries the unit: 100 for centiwatts and for hundredths of a load
// average, 1 for values already in their reporting unit.
std::optional<double> scaled(uint16_t raw, double divisor)
{
    if (raw == kAbsentValue)
        return std::nullopt;
    return raw / divisor;
}

QString field(std::optional<double> value, int precision)
{
    if (!value)
        return {};
    return QString::number(*value, 'f', precision);
}

// A byte-wide reading or index. Zero is a legal backlight level and a legal
// profile index, so absence has its own marker rather than being zero.
std::optional<double> optional8(uint8_t raw)
{
    if (raw == kAbsentPercent)
        return std::nullopt;
    return raw;
}

QJsonValue jsonValue(std::optional<double> value)
{
    if (!value)
        return QJsonValue();
    return QJsonValue(*value);
}

} // namespace

namespace exporter {

QString csvHeader()
{
    return QStringLiteral(
            "ts,pkg_c,board_c,nvme_c,fan_rpm,pkg_w,core_w,uncore_w,cpu_mhz,gpu_mhz,load1,"
            "pkg_throttle,core_throttle,mode_id,pl1_w,pl2_w,max_perf_pct,turbo,epp,hwp_boost,"
            "profile,gt_max_mhz,fan_mode,charge_limit,on_ac,guard,mismatch,degraded,live,mark,"
            "light,brightness,brightness_base,brightness_profile,auto_brightness");
}

QString csvRow(const Sample &s)
{
    const QStringList fields = {
        QString::number(s.ts),
        field(temperature(s.pkgTemp), 1),
        field(temperature(s.boardTemp), 1),
        field(temperature(s.nvmeTemp), 1),
        field(scaled(s.fanRpm, 1.0), 0),
        field(scaled(s.pkgPower, 100.0), 2),
        field(scaled(s.corePower, 100.0), 2),
        field(scaled(s.uncorePower, 100.0), 2),
        field(scaled(s.cpuMhz, 1.0), 0),
        field(scaled(s.gpuMhz, 1.0), 0),
        field(scaled(s.load1, 100.0), 2),
        QString::number(s.pkgThrottle),
        QString::number(s.coreThrottle),
        s.modeId == 0xFF ? QString() : QString::number(s.modeId),
        QString::number(s.pl1W),
        QString::number(s.pl2W),
        QString::number(s.maxPerfPct),
        s.noTurbo ? QStringLiteral("0") : QStringLiteral("1"),
        eppToString(static_cast<Epp>(s.epp)),
        QString::number(s.hwpBoost),
        profileToString(static_cast<Profile>(s.profile)),
        QString::number(s.gtMaxMhz),
        QString::number(s.fanMode),
        QString::number(s.chargeLimit),
        QString::number((s.flags & FlagOnAc) ? 1 : 0),
        QString::number((s.flags & FlagGuardEngaged) ? 1 : 0),
        QString::number((s.flags & FlagWriteMismatch) ? 1 : 0),
        QString::number((s.flags & FlagSensorsDegraded) ? 1 : 0),
        QString::number((s.flags & FlagLiveTuning) ? 1 : 0),
        markToString(static_cast<Mark>(s.mark)),
        field(scaled(s.light, 1.0), 0),
        field(optional8(s.brightness), 0),
        field(optional8(s.brightnessBase), 0),
        field(optional8(s.brightnessProfile), 0),
        QString::number((s.flags & FlagAutoBrightness) ? 1 : 0),
    };
    return fields.join(QLatin1Char(','));
}

QString jsonRow(const Sample &s)
{
    QJsonObject row;
    row[QStringLiteral("ts")] = static_cast<qint64>(s.ts);
    row[QStringLiteral("pkg_c")] = jsonValue(temperature(s.pkgTemp));
    row[QStringLiteral("board_c")] = jsonValue(temperature(s.boardTemp));
    row[QStringLiteral("nvme_c")] = jsonValue(temperature(s.nvmeTemp));
    row[QStringLiteral("fan_rpm")] = jsonValue(scaled(s.fanRpm, 1.0));
    row[QStringLiteral("pkg_w")] = jsonValue(scaled(s.pkgPower, 100.0));
    row[QStringLiteral("core_w")] = jsonValue(scaled(s.corePower, 100.0));
    row[QStringLiteral("uncore_w")] = jsonValue(scaled(s.uncorePower, 100.0));
    row[QStringLiteral("cpu_mhz")] = jsonValue(scaled(s.cpuMhz, 1.0));
    row[QStringLiteral("gpu_mhz")] = jsonValue(scaled(s.gpuMhz, 1.0));
    row[QStringLiteral("load1")] = jsonValue(scaled(s.load1, 100.0));
    row[QStringLiteral("pkg_throttle")] = s.pkgThrottle;
    row[QStringLiteral("core_throttle")] = s.coreThrottle;
    row[QStringLiteral("mode_id")] = s.modeId == 0xFF ? QJsonValue() : QJsonValue(s.modeId);
    row[QStringLiteral("pl1_w")] = s.pl1W;
    row[QStringLiteral("pl2_w")] = s.pl2W;
    row[QStringLiteral("max_perf_pct")] = s.maxPerfPct;
    row[QStringLiteral("turbo")] = s.noTurbo == 0;
    row[QStringLiteral("epp")] = eppToString(static_cast<Epp>(s.epp));
    row[QStringLiteral("hwp_boost")] = s.hwpBoost != 0;
    row[QStringLiteral("profile")] = profileToString(static_cast<Profile>(s.profile));
    row[QStringLiteral("gt_max_mhz")] = s.gtMaxMhz;
    row[QStringLiteral("fan_mode")] = s.fanMode;
    row[QStringLiteral("charge_limit")] = s.chargeLimit;
    row[QStringLiteral("on_ac")] = (s.flags & FlagOnAc) != 0;
    row[QStringLiteral("guard")] = (s.flags & FlagGuardEngaged) != 0;
    row[QStringLiteral("mismatch")] = (s.flags & FlagWriteMismatch) != 0;
    row[QStringLiteral("degraded")] = (s.flags & FlagSensorsDegraded) != 0;
    row[QStringLiteral("live")] = (s.flags & FlagLiveTuning) != 0;
    row[QStringLiteral("mark")] = markToString(static_cast<Mark>(s.mark));
    row[QStringLiteral("light")] = jsonValue(scaled(s.light, 1.0));
    row[QStringLiteral("brightness")] = jsonValue(optional8(s.brightness));
    row[QStringLiteral("brightness_base")] = jsonValue(optional8(s.brightnessBase));
    row[QStringLiteral("brightness_profile")] = jsonValue(optional8(s.brightnessProfile));
    row[QStringLiteral("auto_brightness")] = (s.flags & FlagAutoBrightness) != 0;
    return QString::fromUtf8(QJsonDocument(row).toJson(QJsonDocument::Compact));
}

quint64 write(const MetricsRing &ring, QTextStream &out, Format format, quint32 from, quint32 to)
{
    quint64 written = 0;
    if (format == Format::Csv)
        out << csvHeader() << '\n';
    else
        out << "[\n";

    const quint64 count = ring.count();
    for (quint64 i = 0; i < count; ++i) {
        const Sample sample = ring.at(i);
        // A slot that was never written, or one lost to a truncated tail.
        if (sample.ts == 0)
            continue;
        if (sample.ts < from)
            continue;
        if (to != 0 && sample.ts > to)
            continue;
        if (format == Format::Csv) {
            out << csvRow(sample) << '\n';
        } else {
            if (written > 0)
                out << ",\n";
            out << jsonRow(sample);
        }
        ++written;
    }

    if (format == Format::Json)
        out << (written > 0 ? "\n]\n" : "]\n");
    return written;
}

} // namespace exporter
