---
name: ombra-tuning
description: Tune ombra's mode and brightness definitions from recorded telemetry. Use when asked to analyse a recorded session, to propose new setpoints, or to apply numbers that came out of such an analysis. Covers how to read the export, which conclusions the data does and does not support, and how to apply a change through the daemon.
---

# Tuning ombra from its own telemetry

The daemon records one sample per second and every sample carries the setpoints
that were in force. That is the whole point of the format: a log that does not
say what was applied cannot be analysed. This skill is how that log turns into
numbers, and how those numbers get applied.

**Nothing is applied without the maintainer saying yes.** Propose, wait, then
apply. The commands below change the machine's thermal envelope.

## Getting the data

```bash
ombra status | jq            # what is running right now
ombra config show | jq       # what is saved, and where
ombra export --format csv > session.csv
ombra export --format csv --from $(date -d '2 hours ago' +%s) > recent.csv
```

Membership of the group `ombra` is all it takes; no root, no editing files by
hand. If `ombra` is not on the path the daemon is not installed, and there is
nothing to tune.

Snapshots taken by `scripts/install.sh` before an upgrade live in
`$XDG_STATE_HOME/ombra/telemetry`. The daemon converts a ring in an older
layout rather than discarding it, and a file it cannot read is kept as
`metrics.ring.bak` — so a session that seems to be missing is worth looking for
there before it is written off. That is one slot, replaced by the next
unreadable ring, so it is not an archive.

Samples that came from an older layout carry absences the current one would not
produce: no display count before the field existed, no backlight window before
the automation did. Absent is not zero, and a comparison that treats it as zero
invents a machine with no monitors.

## The columns

Readings: `pkg_c`, `board_c`, `nvme_c`, `fan_rpm`, `pkg_w`, `core_w`,
`uncore_w`, `cpu_mhz`, `gpu_mhz`, `load1`, `displays`, `light_lux`,
`brightness`, `brightness_base`. Setpoints in force: `mode_id`, `pl1_w`,
`pl2_w`, `max_perf_pct`, `turbo`, `epp`, `hwp_boost`, `profile`, `gt_max_mhz`,
`fan_mode`, `brightness_response`, `brightness_floor`. Flags: `on_ac`, `guard`,
`mismatch`, `degraded`, `live`, `auto_brightness`, and `mark`.

`light_lux` is the room in lux, to three decimals — the sensor reads in
thousandths at the bottom of its range, where a dark room lives. `brightness`
is the panel in per cent and `brightness_base` is the baseline the automation
was working around at that moment.

An empty field is a reading that could not be taken. It is not a zero: zero is a
legal fan speed and a legal power draw, and treating the two alike is how a
stopped fan becomes a missing sensor.

Transitions are derived by diffing consecutive rows. There is no event log and
there is not going to be one.

## Rules of reading, each of which cost a session to learn

- **`load1` is not a proxy for load.** Two stretches with the same load average
  drew visibly different power in one recorded session. Do not compare modes by
  binning on `load1`; the same average queue is different work.
- **Modes are only comparable under a controlled protocol.** The one in
  `HARDWARE.md`: eight threads, 90 s to settle, 90 s of recording, cool to 65 °C
  between combinations. Anything else compares two afternoons.
- **Numbers from one scenario are not a default.** This machine has at least two
  — with an external monitor and without — and a setting that was excellent for
  five hours on battery made the graphics stutter the moment a monitor was
  plugged in. Check `displays` before generalising.
- **Mains and battery is a confounder.** Check `on_ac` across the stretch being
  compared; a comparison where one side ran on mains and the other on battery
  measures the charger as much as the setpoints.
- **A gap in `ts` is a suspend.** The first sample after one is not to be
  trusted: the energy counters restart there. Implausible power is recorded as
  absent rather than as a number, but the tick either side is still worth
  skipping.
- **The skin is judged by `board_c`.** `pkg_c` tracks it within a degree at a
  desk, and that is not the scenario where the question arises.
- **The backlight band is `brightness_base ± brightness_response`, and
  saturation at its edges is a signal.** Samples pinned at the top mean the
  automation wanted more room than `response_pct` allows; samples pinned at
  `brightness_floor` mean the floor, not the room, is deciding. Past the curve's
  last point the curve decides nothing, so check where `light_lux` actually
  falls before widening anything.
- **`mismatch` means somebody else is writing the controls.** This daemon owns
  them alone, so a mismatch is either a second writer or a node that refused a
  value.
- **Marks are the subjective axis.** `good`, `too-loud`, `too-slow` are the only
  record of what a person thought. A tuning proposal that ignores them optimises
  temperature and nothing else.

## What the maintainer has decided

Read these before proposing anything; they are decisions, not preferences.

- A cool chassis outweighs performance. When the two conflict, throttle harder.
- Up to 3000 RPM is inaudible, 4000 is audible, 5000–6000 is reachable only on
  explicit demand. A mode names its own fan curve, and may ask for full speed;
  what it must not do is surprise the person with one.
- There are exactly three modes. The middle one is the working mode and is what
  tuning targets; the other two are edges.
- No feedback controller and no automatic mode switching. Static setpoints per
  mode, chosen deliberately.
- The backlight has no profiles. One room, one set of settings: a response, a
  floor and a curve.

## Proposing

A proposal is a table of `was → becomes → what it rests on`, one row per number,
and it names the samples behind each row. A number without a measurement behind
it is a guess, and saying so is part of the proposal.

State the confounders you could not remove. "The comparison held `load1` but not
`on_ac`" is a useful proposal; a table that hides that is not.

## Applying, after the maintainer has agreed

```bash
ombra config set mode work pl1_w=18 pl2_w=26
ombra config set brightness response_pct=30 floor_pct=5 \
    curve="0:0, 1:20, 8:38, 25:50, 60:70, 103:85, 400:100"
ombra config default_mode work
ombra config show | jq          # read back what was written
```

The curve is `lux:position` pairs, lux ascending. There is no profile name in
any of this: the backlight has one set of settings.

The daemon validates with the same code that reads its file, so a value it
refuses is a value the file would have refused. Out-of-range numbers are clamped
rather than rejected, and the reply says what was clamped — read the reply.

To undo everything: `ombra config reset modes` or `ombra config reset all`,
which returns to the values compiled into the daemon.

Live tuning is a different thing and does not survive a reboot:

```bash
ombra set pl1=15 max_perf_pct=40   # immediate, not saved
ombra set fan=max                  # or `ombra fan max`, the same setpoint
ombra config save live to quiet    # keep it
ombra reset                        # or drop it
```

## Calibrating the light curve

The curve is the one part of the backlight that cannot be chosen by eye, and it
is calibrated from the telemetry like any other number here.

**What the curve means.** Each point maps a light reading to a position from 0
to 100, and the position turns into percentage points of the panel:

```
contribution = response_pct × (2 × position / 100 − 1)
```

So position 50 contributes nothing, 0 subtracts the whole response and 100 adds
it. The daemon derives the base by taking the contribution back out of the
level in force, which is what makes returning to the same light return to the
same brightness. Two consequences worth stating outright:

- **Position 50 is not a free choice.** It belongs at the light the room
  usually has, because that is where the screen sits exactly on the level the
  person set. Take it from the distribution of `light_lux` over a long session
  rather than from a round number.
- **Only the shape between points matters, not their count.** Interpolation is
  linear, and flat outside the first and last point.

**What the data says.** Work from stretches where `auto_brightness` is 1:

1. **Manual corrections are the signal.** A change of `brightness` that the
   automation did not make — the level moves and `brightness_base` moves with
   it — is the person saying "in this light, that is wrong". Take `light_lux`
   at that moment and the size of the correction in percentage points; the
   correction is what the curve was wrong by, converted back through the
   formula above.
2. **Saturation is the second signal.** Samples pinned at 100 or at
   `brightness_floor` while the light keeps changing mean the curve is asking
   for more than the scale has. Either the response is too narrow or the
   neutral point is in the wrong place — the two look alike in a histogram and
   are told apart by which end saturates.
3. **A stretch with no corrections and no saturation is a confirmation**, and
   worth as much as a correction. Curve changes proposed against a session that
   never disagreed with the curve are noise.

**What the sensor can tell you.** The measured range on this machine, from the
readings recorded so far: a dark room around 0.3 lux, an ordinary evening room
25 to 100 lux, and a bicycle lamp held a hand's width away 20 855 lux — which
is where the sensor appears to stop. There is no daylight session recorded yet,
so the last point of the curve rests on the sensor's range rather than on
measurement. Say so when proposing it.

**A proposal about the curve** is a table of `lux → position was → becomes`
with the corrections behind each row, plus the count of samples in each light
band. A curve fitted to four corrections in one evening is an observation, not
a calibration; wait for the second session.

## Where the write-ups go

Session analyses go to `tmp/TELEMETRY_<date>.md` with the raw export beside
them. `tmp/` is not tracked and does not travel with a clone, so a conclusion
worth keeping is promoted into `HARDWARE.md` — and only once a second session
has confirmed it. One measurement on one machine is an observation.
