// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

// Deciding where the backlight should be, as a pure function.
//
// The model, in the order the numbers are used:
//
//   base      the level the person set, in percent. Not a level the daemon
//             computed: moving the brightness is a statement about what is
//             wanted in this light, so a hand correction becomes the new base
//             and the window moves with it.
//   window    [base - span, base + span], then cut by the floor and the
//             ceiling. Cut, not shifted: base 90 with a span of 10 and a
//             ceiling of 95 gives 80..95, not 85..95.
//   curve     ambient reading -> a position inside that window, 0 at its
//             bottom and 100 at its top. The curve chooses a point in the
//             window rather than an absolute level, so narrowing the span
//             makes the automation move less instead of pinning it to an edge
//             and ignoring the light.
//   floor,
//   ceiling   absolute bounds. They constrain the automation and never the
//             person: a level set by hand outside them is accepted and becomes
//             the base; the next automatic move lands inside them again.
//
// A backlight that visibly hunts is a defect rather than a tuning problem, and
// four separate mechanisms keep it from happening. Each answers a different
// failure, and none of them substitutes for another:
//
//   quantisation  the target is snapped to a coarse grid, so ordinary drift
//                 produces no write at all;
//   hysteresis    a margin at each grid boundary, without which a target
//                 sitting on one would oscillate between two steps and the
//                 grid would only look like it was working;
//   dwell         a new step has to hold for several ticks, which is what
//                 ignores a hand passing over the sensor or headlights;
//   fade          the move itself is spread over about a second, because a
//                 correct step applied in one write still reads as a defect.
//
// These are constants, not coefficients to be fitted, and every one of them is
// observable in the telemetry: the light reading, the level and the base are
// recorded in every sample, so a bad default shows up in the data.
//
// None of this contradicts the thermal side's refusal of a feedback
// controller. There, measurement showed static setpoints are enough. Here the
// input is the room, which changes continuously and not at the daemon's
// request; there is nothing static to follow it with.

// Percentage points of the panel scale. Coarse on purpose: this is the "wide
// steps" the automation is allowed to move in.
inline constexpr int kBrightnessStepPct = 5;
// A third of a step, expressed so the comparison stays in integers:
// 6 * distance >= 5 * step is distance >= step/2 + step/3.
inline constexpr int kBrightnessHysteresisNumerator = 5;
inline constexpr int kBrightnessHysteresisDenominator = 6;
// Ticks a new step must survive before anything moves. The daemon ticks at
// 1 Hz, so this is seconds.
inline constexpr int kBrightnessDwellTicks = 5;
// The move itself: this many writes, this far apart, so a commit takes about
// a second whatever the distance. Small distances finish sooner, because the
// step never drops below one unit of the panel's own scale.
inline constexpr int kBrightnessFadeSteps = 25;
inline constexpr int kBrightnessFadeIntervalMs = 40;

// Percentage points either side of the level the person set. Twenty rather than
// ten because the narrower window was measured saturating: over a recorded
// evening the automation spent 13% of its time pinned to the top of a ±10
// window and none at the bottom, which is the automation asking for more room.
inline constexpr int kBrightnessDefaultSpanPct = 20;
inline constexpr int kBrightnessDefaultFloorPct = 5;
inline constexpr int kBrightnessDefaultCeilingPct = 100;

// One point of the response curve: an ambient reading in the sensor's own
// units (see light_sensor.h — they are not lux) and where in the window it
// puts the backlight, 0 for the bottom of the window and 100 for the top.
struct BrightnessPoint {
    int light = 0;
    int position = 0;
};

struct BrightnessProfile {
    QString id;
    QString label;
    // How far the automation may wander from the base, in percentage points,
    // symmetrically.
    int spanPct = kBrightnessDefaultSpanPct;
    int floorPct = kBrightnessDefaultFloorPct;
    int ceilingPct = kBrightnessDefaultCeilingPct;
    QList<BrightnessPoint> curve;
};

struct BrightnessWindow {
    int low = 0;
    int high = 0;
};

BrightnessWindow brightnessWindow(const BrightnessProfile &profile, int base);

// Linear between points, flat outside the first and the last. An empty curve
// has no answer and yields the bottom of the window.
int brightnessCurvePosition(const QList<BrightnessPoint> &curve, int light);

struct BrightnessState {
    // Percent, or kBrightnessUnset before the panel has been read.
    static constexpr int kUnset = -1;

    int base = kUnset;
    // The grid position the automation currently holds. Unset means the next
    // decision only adopts a position and moves nothing — which is what makes
    // switching the automation on, and correcting the level by hand, leave the
    // backlight exactly where it is.
    int step = kUnset;
    int pending = kUnset;
    int pendingTicks = 0;
};

struct BrightnessDecision {
    bool move = false;
    int target = 0;
    BrightnessWindow window;
};

// Advances the state by one tick and says whether to move. Absent light means
// no reading was available, and the answer is then to hold: darkness and a
// missing sensor are not the same thing.
BrightnessDecision decideBrightness(const BrightnessProfile &profile, std::optional<int> light,
                                    BrightnessState &state);

// The brightness profiles, read from the same configuration file as the modes
// but kept as a separate list with a separate selection. Nothing about the
// thermal mode may reach this side, in either direction.
class BrightnessProfileTable {
public:
    static BrightnessProfileTable builtinDefaults();

    // Returns false and leaves the table untouched when the file defines no
    // usable profile, exactly like the mode table.
    bool load(const QString &path, QStringList *problems = nullptr);

    const QList<BrightnessProfile> &profiles() const { return m_profiles; }
    const BrightnessProfile *find(const QString &id) const;
    int indexOf(const QString &id) const;
    const QString &defaultProfileId() const { return m_defaultProfileId; }

    // The configuration text that would reproduce a profile, for the tray's
    // "copy as a configuration snippet". The curve is written out unchanged:
    // it is not tunable from the tray, and a section missing it would not be
    // a section that could be pasted into the file.
    static QString toConfigSnippet(const BrightnessProfile &profile);

private:
    QList<BrightnessProfile> m_profiles;
    QString m_defaultProfileId;
};
