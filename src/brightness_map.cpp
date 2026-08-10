// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "brightness_map.h"

#include <QFileInfo>
#include <QSettings>

#include <algorithm>

namespace {

int clampReported(int value, int low, int high, const QString &what, QStringList *problems)
{
    const int clamped = std::clamp(value, low, high);
    if (clamped != value && problems) {
        problems->append(QStringLiteral("%1: %2 is outside %3..%4, using %5")
                                 .arg(what)
                                 .arg(value)
                                 .arg(low)
                                 .arg(high)
                                 .arg(clamped));
    }
    return clamped;
}

// Lux in the file, millilux inside. A person reads and writes lux — it is the
// unit rooms are described in — and the dark end needs the fraction: 0.3 lux
// and 3 lux are different rooms.
quint32 milliluxFromText(const QString &text, bool *ok)
{
    const double lux = text.trimmed().toDouble(ok);
    if (!*ok || lux < 0)
        return 0;
    return static_cast<quint32>(std::min(lux * 1000.0, 4294967000.0));
}

QString luxText(quint32 millilux)
{
    QString text = QString::number(millilux / 1000.0, 'f', 3);
    // Trailing zeroes on a light level are noise, and the file is read by
    // people as well as by the parser.
    while (text.endsWith(QLatin1Char('0')))
        text.chop(1);
    if (text.endsWith(QLatin1Char('.')))
        text.chop(1);
    return text;
}

// "0:0, 25:50, 400:100" — a light level in lux and what the room is worth
// there, from 0 for "subtract the whole response" through 50 for "contribute
// nothing" to 100 for "add all of it". Malformed points are dropped with a line
// in `problems` rather than rejecting the curve: one typo must not switch the
// automation into something unpredictable.
QList<BrightnessPoint> parseCurve(const QString &text, const QString &what, QStringList *problems)
{
    QList<BrightnessPoint> curve;
    const QStringList entries = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &entry : entries) {
        const QStringList halves = entry.trimmed().split(QLatin1Char(':'));
        bool lightOk = false;
        bool positionOk = false;
        if (halves.size() == 2) {
            const quint32 light = milliluxFromText(halves.at(0), &lightOk);
            const int position = halves.at(1).trimmed().toInt(&positionOk);
            if (lightOk && positionOk) {
                curve.append({ light, std::clamp(position, 0, 100) });
                continue;
            }
        }
        if (problems)
            problems->append(QStringLiteral("%1: '%2' is not lux:position").arg(what, entry));
    }

    std::sort(curve.begin(), curve.end(),
              [](const BrightnessPoint &a, const BrightnessPoint &b) { return a.light < b.light; });
    return curve;
}

} // namespace

BrightnessSettings builtinBrightnessSettings()
{
    BrightnessSettings settings;
    settings.responsePct = kBrightnessDefaultResponsePct;
    settings.floorPct = kBrightnessDefaultFloorPct;
    // Lux, and measured on this machine rather than guessed:
    //
    //   0.3   the darkest reading of a recorded night
    //   25    an evening with the desk lamps on and the ceiling light off —
    //         the light this room usually has, which is why the curve is
    //         neutral there
    //   103   the same room with the ceiling light on
    //   400   past anything measured indoors here; the curve is flat above it
    //
    // The top is deliberately short of the sensor's own reach of about
    // 20 000 lux: a daylight session at a window has not been recorded yet, and
    // a point invented for it would be a guess wearing a measurement's clothes.
    settings.curve = {
        { 0, 0 }, { 1000, 20 }, { 8000, 38 }, { 25000, 50 },
        { 60000, 70 }, { 103000, 85 }, { 400000, 100 },
    };
    return settings;
}

bool applyBrightnessSetting(BrightnessSettings &settings, const QString &key,
                            const QString &value, QString *error, QStringList *clamped)
{
    const QString what = QStringLiteral("brightness: %1").arg(key);

    if (key == QLatin1String("curve")) {
        QStringList curveProblems;
        const QList<BrightnessPoint> curve = parseCurve(value, what, &curveProblems);
        if (curve.isEmpty()) {
            if (error)
                *error = QStringLiteral("%1: no usable points").arg(what);
            return false;
        }
        if (clamped)
            clamped->append(curveProblems);
        settings.curve = curve;
        return true;
    }

    bool ok = false;
    const int number = value.trimmed().toInt(&ok);
    if (!ok) {
        if (error)
            *error = QStringLiteral("%1: '%2' is not a number").arg(what, value);
        return false;
    }
    if (key == QLatin1String("response_pct")) {
        settings.responsePct = clampReported(number, 0, 100, what, clamped);
        return true;
    }
    if (key == QLatin1String("floor_pct")) {
        settings.floorPct = clampReported(number, 0, 100, what, clamped);
        return true;
    }

    if (error)
        *error = QStringLiteral("brightness: unknown setting '%1'").arg(key);
    return false;
}

bool loadBrightnessSettings(const QString &path, BrightnessSettings *settings,
                            QStringList *problems)
{
    if (!QFileInfo::exists(path))
        return false;
    QSettings file(path, QSettings::IniFormat);
    if (file.status() != QSettings::NoError)
        return false;
    if (!file.childGroups().contains(QStringLiteral("brightness")))
        return false;

    BrightnessSettings parsed = builtinBrightnessSettings();
    file.beginGroup(QStringLiteral("brightness"));
    for (const QString &key : file.childKeys()) {
        QString error;
        // QSettings reads an unquoted comma-separated value as a list, and
        // toString() on a list is empty. Rejoining is what makes the curve
        // readable whether or not the file happens to quote it.
        const QString value = file.value(key).toStringList().join(QLatin1Char(','));
        if (!applyBrightnessSetting(parsed, key.toLower(), value, &error, problems) && problems)
            problems->append(error + QStringLiteral(", ignored"));
    }
    file.endGroup();

    *settings = parsed;
    return true;
}

QString brightnessSettingsSection(const BrightnessSettings &settings)
{
    QStringList points;
    for (const BrightnessPoint &point : settings.curve)
        points.append(QStringLiteral("%1:%2").arg(luxText(point.light)).arg(point.position));

    QString text;
    text += QStringLiteral("[brightness]\n");
    text += QStringLiteral("response_pct=%1\n").arg(settings.responsePct);
    text += QStringLiteral("floor_pct=%1\n").arg(settings.floorPct);
    // Lux, and the one place a light level is written down for a person to
    // read. The parser takes the same text back.
    text += QStringLiteral("curve=%1\n").arg(points.join(QStringLiteral(", ")));
    return text;
}

int brightnessCurvePosition(const QList<BrightnessPoint> &curve, quint32 light)
{
    if (curve.isEmpty())
        return 0;
    if (light <= curve.first().light)
        return curve.first().position;
    if (light >= curve.last().light)
        return curve.last().position;

    for (int i = 1; i < curve.size(); ++i) {
        const BrightnessPoint &high = curve.at(i);
        if (light > high.light)
            continue;
        const BrightnessPoint &low = curve.at(i - 1);
        const qint64 span = static_cast<qint64>(high.light) - low.light;
        if (span <= 0)
            return high.position;
        const qint64 rise = static_cast<qint64>(high.position - low.position)
                * (static_cast<qint64>(light) - low.light);
        return low.position + static_cast<int>((rise + span / 2) / span);
    }
    return curve.last().position;
}

int brightnessContribution(const BrightnessSettings &settings, std::optional<quint32> light)
{
    if (!light || settings.curve.isEmpty())
        return 0;
    // Position 50 contributes nothing, 0 subtracts the whole response and 100
    // adds it. Rounded away from zero so that a contribution of half a point
    // does not silently vanish.
    const int position = brightnessCurvePosition(settings.curve, *light);
    const int scaled = (2 * position - 100) * settings.responsePct;
    return scaled >= 0 ? (scaled + 50) / 100 : -((-scaled + 50) / 100);
}

int brightnessBaseFor(const BrightnessSettings &settings, int level,
                      std::optional<quint32> light)
{
    // The floor first: what got applied is what the base is derived from, and a
    // level nobody can see would make a base nobody can explain.
    const int applied = std::clamp(level, settings.floorPct, 100);
    return applied - brightnessContribution(settings, light);
}

BrightnessWindow brightnessWindow(const BrightnessSettings &settings, int base)
{
    BrightnessWindow window;
    window.low = std::clamp(base - settings.responsePct, settings.floorPct, 100);
    window.high = std::clamp(base + settings.responsePct, settings.floorPct, 100);
    return window;
}

BrightnessDecision decideBrightness(const BrightnessSettings &settings,
                                    std::optional<quint32> light, BrightnessState &state)
{
    BrightnessDecision decision;
    if (state.base == BrightnessState::kUnset)
        return decision;

    decision.window = brightnessWindow(settings, state.base);
    decision.target = state.step == BrightnessState::kUnset ? state.base : state.step;

    // No reading is not darkness. Without one the automation holds where it
    // is, the same way the thermal side falls back rather than guessing.
    if (!light || settings.curve.isEmpty())
        return decision;

    const int wanted = std::clamp(state.base + brightnessContribution(settings, light),
                                  settings.floorPct, 100);

    // Snap to the grid first, then keep it inside what is permitted: the floor
    // and the top of the scale are legitimate resting places even when they are
    // not on the grid.
    int candidate = (wanted + kBrightnessStepPct / 2) / kBrightnessStepPct * kBrightnessStepPct;
    candidate = std::clamp(candidate, settings.floorPct, 100);

    if (state.step == BrightnessState::kUnset) {
        // Adopt the position without moving. This is what makes switching the
        // automation on — and correcting the level by hand — leave the
        // backlight alone until the light itself changes.
        state.step = candidate;
        state.pending = BrightnessState::kUnset;
        state.pendingTicks = 0;
        decision.target = std::clamp(state.base, settings.floorPct, 100);
        return decision;
    }

    decision.target = state.step;

    if (candidate == state.step) {
        state.pending = BrightnessState::kUnset;
        state.pendingTicks = 0;
        return decision;
    }

    // Hysteresis: a candidate that has not cleared the boundary by a margin is
    // the same step as far as this is concerned. Without it a target sitting on
    // a boundary would oscillate and the coarse grid would only look like it
    // was working.
    if (std::abs(candidate - state.step) * kBrightnessHysteresisDenominator
        < kBrightnessStepPct * kBrightnessHysteresisNumerator) {
        state.pending = BrightnessState::kUnset;
        state.pendingTicks = 0;
        return decision;
    }

    if (candidate != state.pending) {
        state.pending = candidate;
        state.pendingTicks = 0;
    }
    // The dwell: a new step has to hold, which is what ignores a hand passing
    // over the sensor.
    if (++state.pendingTicks < kBrightnessDwellTicks)
        return decision;

    state.step = candidate;
    state.pending = BrightnessState::kUnset;
    state.pendingTicks = 0;
    decision.move = true;
    decision.target = candidate;
    return decision;
}
