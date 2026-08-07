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

// "0:0, 40:15, 150:40" — an ambient reading and where in the window it puts
// the backlight. Malformed points are dropped with a line in `problems`
// rather than rejecting the profile: one typo must not switch the automation
// into something unpredictable.
QList<BrightnessPoint> parseCurve(const QString &text, const QString &what, QStringList *problems)
{
    QList<BrightnessPoint> curve;
    const QStringList entries = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &entry : entries) {
        const QStringList halves = entry.trimmed().split(QLatin1Char(':'));
        bool lightOk = false;
        bool positionOk = false;
        if (halves.size() == 2) {
            const int light = halves.at(0).trimmed().toInt(&lightOk);
            const int position = halves.at(1).trimmed().toInt(&positionOk);
            if (lightOk && positionOk) {
                curve.append({ std::max(0, light), std::clamp(position, 0, 100) });
                continue;
            }
        }
        if (problems)
            problems->append(QStringLiteral("%1: '%2' is not light:position").arg(what, entry));
    }

    std::sort(curve.begin(), curve.end(),
              [](const BrightnessPoint &a, const BrightnessPoint &b) { return a.light < b.light; });
    return curve;
}

} // namespace

BrightnessWindow brightnessWindow(const BrightnessProfile &profile, int base)
{
    // The span is measured from the base, and only then do the floor and the
    // ceiling cut the result. Applying them the other way round would move the
    // window instead of trimming it.
    const int floorPct = std::min(profile.floorPct, profile.ceilingPct);
    BrightnessWindow window;
    window.low = std::clamp(base - profile.spanPct, floorPct, profile.ceilingPct);
    window.high = std::clamp(base + profile.spanPct, floorPct, profile.ceilingPct);
    return window;
}

int brightnessCurvePosition(const QList<BrightnessPoint> &curve, int light)
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
        const int span = high.light - low.light;
        if (span <= 0)
            return high.position;
        const qint64 rise = static_cast<qint64>(high.position - low.position) * (light - low.light);
        return low.position + static_cast<int>((rise + span / 2) / span);
    }
    return curve.last().position;
}

BrightnessDecision decideBrightness(const BrightnessProfile &profile, std::optional<int> light,
                                    BrightnessState &state)
{
    BrightnessDecision decision;
    if (state.base == BrightnessState::kUnset)
        return decision;

    decision.window = brightnessWindow(profile, state.base);
    decision.target = state.step == BrightnessState::kUnset ? state.base : state.step;

    // No reading is not darkness. Without one the automation holds where it
    // is, the same way the thermal side falls back rather than guessing.
    if (!light || profile.curve.isEmpty())
        return decision;

    const int position = brightnessCurvePosition(profile.curve, *light);
    const int width = decision.window.high - decision.window.low;
    const int wanted =
            decision.window.low + static_cast<int>((static_cast<qint64>(width) * position + 50)
                                                   / 100);

    // Snap to the grid first, then keep it inside the window: the window's own
    // edges are legitimate resting places even when they are not on the grid.
    int candidate = (wanted + kBrightnessStepPct / 2) / kBrightnessStepPct * kBrightnessStepPct;
    candidate = std::clamp(candidate, decision.window.low, decision.window.high);

    if (state.step == BrightnessState::kUnset) {
        // Adopt the position without moving. This is what makes switching the
        // automation on — and correcting the level by hand — leave the
        // backlight alone until the light itself changes.
        state.step = candidate;
        state.pending = BrightnessState::kUnset;
        state.pendingTicks = 0;
        decision.target = state.base;
        return decision;
    }

    decision.target = state.step;

    if (candidate == state.step) {
        state.pending = BrightnessState::kUnset;
        state.pendingTicks = 0;
        return decision;
    }

    // A target sitting on a grid boundary would flip between two steps on
    // sensor noise alone. It has to clear the boundary by a margin before the
    // step counts as changed.
    if (kBrightnessHysteresisDenominator * std::abs(wanted - state.step)
        < kBrightnessHysteresisNumerator * kBrightnessStepPct) {
        state.pending = BrightnessState::kUnset;
        state.pendingTicks = 0;
        return decision;
    }

    if (candidate != state.pending) {
        state.pending = candidate;
        state.pendingTicks = 0;
    }
    if (++state.pendingTicks < kBrightnessDwellTicks)
        return decision;

    state.step = candidate;
    state.pending = BrightnessState::kUnset;
    state.pendingTicks = 0;
    decision.move = true;
    decision.target = candidate;
    return decision;
}

BrightnessProfileTable BrightnessProfileTable::builtinDefaults()
{
    BrightnessProfileTable table;

    // One profile, because one is what this machine needs. The curve is a
    // starting point calibrated by eye and meant to be replaced from recorded
    // telemetry; the readings are the sensor's own units, not lux.
    BrightnessProfile indoors;
    indoors.id = QStringLiteral("indoors");
    indoors.label = QStringLiteral("Indoors");
    indoors.spanPct = kBrightnessDefaultSpanPct;
    indoors.floorPct = kBrightnessDefaultFloorPct;
    indoors.ceilingPct = kBrightnessDefaultCeilingPct;
    indoors.curve = {
        { 0, 0 }, { 500, 20 }, { 1500, 45 }, { 3000, 75 }, { 6000, 100 },
    };

    table.m_profiles = { indoors };
    table.m_defaultProfileId = indoors.id;
    return table;
}

bool BrightnessProfileTable::load(const QString &path, QStringList *problems)
{
    if (!QFileInfo::exists(path))
        return false;

    QSettings settings(path, QSettings::IniFormat);
    if (settings.status() != QSettings::NoError)
        return false;

    const BrightnessProfile fallback = builtinDefaults().profiles().first();
    QList<BrightnessProfile> parsed;

    const QStringList groups = settings.childGroups();
    for (const QString &group : groups) {
        // "[brightness <id>]". A slash would be read by QSettings as a group
        // separator, so the separator here is a space, as it is for modes.
        if (!group.startsWith(QLatin1String("brightness ")))
            continue;
        const QString id = group.mid(11).trimmed();
        if (id.isEmpty()) {
            if (problems)
                problems->append(QStringLiteral("a brightness section has an empty name, skipped"));
            continue;
        }

        settings.beginGroup(group);

        BrightnessProfile profile;
        profile.id = id;
        profile.label = settings.value(QStringLiteral("label"), id).toString();
        profile.spanPct =
                clampReported(settings.value(QStringLiteral("span_pct"), kBrightnessDefaultSpanPct)
                                      .toInt(),
                              0, 100, QStringLiteral("brightness %1: span_pct").arg(id), problems);
        profile.floorPct = clampReported(
                settings.value(QStringLiteral("floor_pct"), kBrightnessDefaultFloorPct).toInt(), 0,
                100, QStringLiteral("brightness %1: floor_pct").arg(id), problems);
        profile.ceilingPct = clampReported(
                settings.value(QStringLiteral("ceiling_pct"), kBrightnessDefaultCeilingPct).toInt(),
                profile.floorPct, 100, QStringLiteral("brightness %1: ceiling_pct").arg(id),
                problems);
        // QSettings reads an unquoted comma-separated value as a list, and
        // toString() on a list is empty. Rejoining is what makes the curve
        // readable whether or not the file happens to quote it.
        profile.curve = parseCurve(
                settings.value(QStringLiteral("curve")).toStringList().join(QLatin1Char(',')),
                QStringLiteral("brightness %1: curve").arg(id), problems);
        if (profile.curve.isEmpty()) {
            if (problems) {
                problems->append(QStringLiteral("brightness %1: no usable curve, using the "
                                                "built-in one")
                                         .arg(id));
            }
            profile.curve = fallback.curve;
        }

        parsed.append(profile);
        settings.endGroup();
    }

    if (parsed.isEmpty())
        return false;

    std::sort(parsed.begin(), parsed.end(),
              [](const BrightnessProfile &a, const BrightnessProfile &b) { return a.id < b.id; });
    m_profiles = parsed;

    // QSettings folds a "[general]" section into the root of the file, so the
    // key is read without its section name.
    const QString wanted =
            settings.value(QStringLiteral("default_brightness_profile")).toString().trimmed();
    if (!wanted.isEmpty() && find(wanted)) {
        m_defaultProfileId = wanted;
    } else {
        if (!wanted.isEmpty() && problems) {
            problems->append(QStringLiteral("default_brightness_profile '%1' is not a defined "
                                            "profile, using %2")
                                     .arg(wanted, m_profiles.first().id));
        }
        m_defaultProfileId = m_profiles.first().id;
    }

    return true;
}

const BrightnessProfile *BrightnessProfileTable::find(const QString &id) const
{
    for (const BrightnessProfile &profile : m_profiles) {
        if (profile.id == id)
            return &profile;
    }
    return nullptr;
}

int BrightnessProfileTable::indexOf(const QString &id) const
{
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles.at(i).id == id)
            return i;
    }
    return -1;
}

QString BrightnessProfileTable::toConfigSnippet(const BrightnessProfile &profile)
{
    QStringList points;
    for (const BrightnessPoint &point : profile.curve)
        points.append(QStringLiteral("%1:%2").arg(point.light).arg(point.position));

    return QStringLiteral("[brightness %1]\n"
                          "label=%2\n"
                          "span_pct=%3\n"
                          "floor_pct=%4\n"
                          "ceiling_pct=%5\n"
                          "curve=%6\n")
            .arg(profile.id, profile.label)
            .arg(profile.spanPct)
            .arg(profile.floorPct)
            .arg(profile.ceilingPct)
            .arg(points.join(QStringLiteral(", ")));
}
