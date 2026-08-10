// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <optional>

// Deciding where the backlight should be, as a pure function.
//
// The model in one line: **the light adds or subtracts from the level you set.**
//
//   contribution  what the room is worth right now, in percentage points, from
//                 minus the response to plus it. Dark rooms subtract, bright
//                 ones add, and the light at which it is zero is the light the
//                 curve calls neutral.
//   base          the level you asked for, with the contribution of the moment
//                 taken back out. Derived rather than stored as typed: that is
//                 what makes returning to the same light return to the same
//                 level.
//   response      how far the automation may move. One number, symmetric
//                 around the base until an end of the scale cuts it.
//   floor         the level nothing goes below — not the automation, and not a
//                 slip of the hand either. There is no ceiling: that is 100.
//
// Worked through, with a response of 30 and the neutral light at 25 lux:
//
//   you set 80 % in a room the curve scores at −3   base = 83
//   the room brightens and the curve scores +21     target = 100 (capped)
//   the room returns to what it was                 target = 80, exactly
//
// The obvious alternative — keep the level as the base and let the curve pick a
// point inside a window around it — loses the choice at the first change of
// light: coming back to the same room gives a different number, because the
// curve's answer inside the window has nothing to do with what was picked.
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
// How far the target has to be from the step in force before another step is
// considered, as a fraction of a step and in integers so the comparison needs no
// floating point: 6 * distance >= 5 * step is distance >= step/2 + step/3.
//
// Measured against the target as the curve produced it, not against the target
// snapped to the grid. Snapping first makes this a no-op, because two adjacent
// steps are a whole step apart and clear any margin below one.
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

// How far the automation may move from the level you set, in percentage points.
// Taken from recorded sessions rather than by eye: a narrower response spends
// its time pinned at an end of the band, which the telemetry shows plainly.
inline constexpr int kBrightnessDefaultResponsePct = 30;
// Nothing goes below this, including a person who drags a slider too far. The
// point of the floor is that a screen driven into unreadable is a failure and
// not a saving.
inline constexpr int kBrightnessDefaultFloorPct = 5;

// One point of the response curve.
struct BrightnessPoint {
    // Millilux, so the dark end keeps its resolution: a room at 0.3 lux and one
    // at 3 lux are different rooms, and whole lux cannot tell them apart.
    quint32 light = 0;
    // 0 subtracts the whole response, 50 contributes nothing, 100 adds the
    // whole response. **50 is therefore not a free choice**: it is the light at
    // which the screen sits exactly on the level you set, and it belongs on the
    // light your room usually has. Calibrated from recorded telemetry, never by
    // eye.
    int position = 0;
};

// Everything the backlight automation is configured with. One of these, not a
// list: a profile would be a name for a set of these numbers, and with one room
// there is never a second set to switch to.
struct BrightnessSettings {
    int responsePct = kBrightnessDefaultResponsePct;
    int floorPct = kBrightnessDefaultFloorPct;
    QList<BrightnessPoint> curve;
};

// Applies one "key=value" of the settings vocabulary, on the same terms as
// applyModeSetting(): one function for the file and for the socket, an unknown
// key or unusable value refused through `error`, a number outside its range
// clamped with a line in `clamped`.
bool applyBrightnessSetting(BrightnessSettings &settings, const QString &key,
                            const QString &value, QString *error, QStringList *clamped);

// Reads the "[brightness]" section of the settings file. False leaves the
// argument untouched, which is how a missing or unusable file falls back to the
// built-in values.
bool loadBrightnessSettings(const QString &path, BrightnessSettings *settings,
                            QStringList *problems = nullptr);

// The section as it appears in the settings file. The writing half of
// loadBrightnessSettings(), and what the round-trip test asserts.
QString brightnessSettingsSection(const BrightnessSettings &settings);

BrightnessSettings builtinBrightnessSettings();

// What the room is worth right now, in percentage points: negative in the dark,
// positive in the bright, zero at the light the curve calls neutral. An empty
// curve or a missing reading contributes nothing.
int brightnessContribution(const BrightnessSettings &settings, std::optional<quint32> light);

// The level you asked for with the contribution of the moment taken back out,
// so that the same light later reproduces the same level. A level below the
// floor never becomes a base: the floor is what actually got applied, and a
// base derived from a level nobody can see is a state with no home on screen.
int brightnessBaseFor(const BrightnessSettings &settings, int level,
                      std::optional<quint32> light);

// Where the automation may put the backlight, for display. Cut by the floor and
// by the end of the scale, so it is what will happen rather than what the
// arithmetic wanted.
struct BrightnessWindow {
    int low = 0;
    int high = 0;
};
BrightnessWindow brightnessWindow(const BrightnessSettings &settings, int base);

// Linear between points, flat outside the first and the last.
int brightnessCurvePosition(const QList<BrightnessPoint> &curve, quint32 light);

struct BrightnessState {
    // Percent, or kUnset before the panel has been read.
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
BrightnessDecision decideBrightness(const BrightnessSettings &settings,
                                    std::optional<quint32> light, BrightnessState &state);
