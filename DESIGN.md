*English · [Русский](DESIGN.ru.md)*

# How this is built, and why

Decisions about the shape of the project, with the reasoning that produced them.
They are not frozen. Each one is here so that changing it is an argument with a
reason rather than a discovery that somebody once thought about it — and so that
a change to one can be checked against the others.

What is frozen is the goal, and it is in `AGENTS.md`: a cool chassis outweighs
performance, noise has a calibrated ceiling, and the hardware safety rules are
not negotiable. Everything below serves that goal and is answerable to it.

Two things follow from that. A decision here changes by an ordinary edit,
together with the code, the documents and the Russian copies. And a decision
that turned out wrong is rewritten rather than annotated: this file says what
the project is, not what it used to be.

## Architecture

- **Exactly one owner of the thermal controls: this daemon.** Two processes
  writing the same sysfs nodes produce race-dependent behaviour, not
  redundancy. The vendor daemon is masked with
  `systemctl mask com.system76.PowerDaemon.service` — its real unit name, not
  the `system76-power.service` alias, because masking an alias leaves the real
  unit enabled and it returns at the next boot. Layering on top of it, and
  forking its source, are both rejected.
- **Installing the package does not take ownership.** It creates the group the
  tray needs, prints the two commands that hand the controls over, and stops
  there — it enables nothing, starts nothing and masks nothing. Transferring
  ownership of the thermal controls is a deliberate act with a rollback
  procedure, not a side effect of `dpkg -i`, and the packaging test asserts it.
- The daemon answers on `net.hadess.PowerProfiles` and
  `org.freedesktop.UPower.PowerProfiles` (the standard public interface, and the
  primary one) and on `com.system76.PowerDaemon` (vendor-specific, for the
  COSMIC panel applet). The stock D-Bus policy already permits a root process to
  own all three; it does not need editing.
- **Display brightness is never coupled to the thermal mode.** Altering the
  backlight on a profile switch is `system76-power` behaviour that this project
  treats as a defect, and restoring parity with the original on that point is
  not an improvement. Selecting a mode must leave the backlight exactly where
  it was.
- **Brightness that follows the ambient light sensor is a separate function
  with its own switch.** It reacts to the room, not to the power profile, which
  is why it does not contradict the rule above. Nothing about the thermal mode
  may reach it, in either direction.
- **The light adds to or subtracts from the level the person set.** A hand
  correction is an instruction, not an interruption: moving the brightness
  states what is wanted in this light, and the daemon takes the room's
  contribution of that moment back out of it to get the baseline. That is what
  makes the same light return the same level. One number says how far the room
  may move it either way, a floor says what nothing goes below, and there is no
  ceiling — that is 100 %. The floor binds the person as well, because a screen
  driven into unreadable is a failure and not a saving.
- **There are no brightness profiles.** One room, one set of settings: a
  response, a floor and a curve. A profile would be a name for a set of those
  numbers, and there was never a second set to switch to.
- **The curve is calibrated from telemetry, not chosen by eye.** It maps lux to
  a position from 0 to 100, and the position at which it contributes nothing
  belongs at the light the room usually has. Nothing about it is on a slider.
- **A backlight that visibly hunts is a defect, not a tuning problem.** The
  automation is watched by the person it serves, all day, and one wrong step
  costs more attention than the whole feature saves. It moves in coarse steps
  with hysteresis at their boundaries, waits before acting on a change, and
  fades rather than jumps. Steady light must produce no writes at all.
- **The automation is off by default, switches off in one action, and stays
  off across a reboot.** Something the person turned off must not come back on
  its own. Switching it on or off does not move the backlight — a jump in
  brightness in answer to a toggle is the same defect as one in answer to a
  profile switch.
- The panel's three slots are redefined as this project's modes. **The middle
  mode is the working one — the machine spends the overwhelming majority of its
  time there.** It is applied at boot and it is the mode that telemetry tuning
  targets. The other two are edge cases.
- **The fan is a setpoint like the others.** A mode names one of the firmware's
  three curves and may ask for full speed (`pwm1_enable=0`); the console offers
  the four as one list, so "it is too hot right now" is one click. What is
  forbidden is the machine going there unasked — the emergency guard excepted.
- Responsibilities inherited along with ownership, and therefore mandatory:
  reacting to AC/battery transitions, and the charge threshold at
  `/sys/class/power_supply/BAT0/charge_control_end_threshold`.

## No automatic regulation

The daemon applies **static setpoints per mode**. It does not run a feedback
loop that continuously adjusts the power limit, and it does not switch modes by
itself.

- **No feedback controller.** Measurement so far shows that static setpoints
  handle both the idle case and, apparently, sustained load. Build a controller
  only if a measurement demonstrates that static setpoints are simultaneously
  too restrictive and too loud in some real situation. Until then, a controller
  is unjustified machinery with coefficients to tune and failure modes — hunting
  and sticking — that static setpoints cannot have.
- **No automatic mode switching by context.** Selecting a mode from the panel or
  the tray is a deliberate act. Switching on microphone or camera activity is a
  reasonable idea that stays deferred until the manual workflow has produced
  data.
- **This does not remove the emergency thermal guard.** A hard clamp that cuts
  power when the package approaches the hardware cutout is a safety interlock,
  not regulation, and it is always active regardless of mode. Given this
  platform's history of hardware thermal shutdowns, removing it is never a
  simplification.
- **Reproducing the vendor's stock behaviour is not a mode.** That behaviour is
  the problem this project exists to fix. The escape hatch, if the modes here
  turn out worse, is `systemctl unmask com.system76.PowerDaemon.service` — no
  code required.

## Settings

- **The daemon owns its settings.** Defaults are compiled in; what has been
  changed lives in a text file the daemon writes, `/var/lib/ombra/settings.conf`,
  and nothing ships a file in `/etc`. The reason is not convenience: mode
  parameters are tuned by automated analysis of recorded telemetry, and the
  tooling that does the analysing has to be able to apply what it found —
  without root, and without operating a dialog.
- **Editing goes through the daemon and nowhere else.** `ombra config` and the
  tray send the same commands, values are validated by the same code that reads
  the file, and there is exactly one writer. The file is machine-written: hand
  comments in it do not survive a change, which is why the vocabulary is
  documented in `README.md` instead. The selected mode and the other runtime
  choices stay in a separate state file, because they have a different lifetime.
  Reloading a file changed from outside is still driven by inotify, without a
  restart.
- **There are exactly three modes and they are edited in place.** The desktop
  panel has three slots; a fourth definition would exist only where the panel
  cannot reach it. Creating and deleting modes is therefore not a feature.
- **A reset restores the built-in values**, per group and on demand. It is the
  way out of a mess and the state of a fresh install, not a routine operation.
- **What is applied and what is written down are two properties of one thing.**
  Moving a control applies it at once and writes nothing; the mode selector
  empties, because what is running is then not one of the three. Saving is a
  separate act that names the mode the numbers land in. This is one tab, not
  two: an arrangement that splits "applies now" from "written down" shows the
  same numbers twice and makes the person hold the difference in their head.
- **The tray never writes the settings file itself.** It asks the daemon, on the
  same commands the command line sends. Two writers of one file is the same
  mistake as two owners of one control surface.
- **The panel icon is the desktop's to draw, and the console's palette is the
  person's to choose.** The icon is published as a standard theme name so the
  panel draws it from the icon theme in force and recolours it to match itself —
  but only when that theme has the name, because a host prefers a name over any
  picture sent with it and draws a question mark for one it cannot find. Where
  the name is missing the icon is a picture of ours in fixed colours, never in
  palette colours: the palette is the application's and the panel is the
  desktop's, and they disagree exactly when the desktop is light with a dark bar.
  The window's own light or dark is a setting on the settings tab, because a
  panel-and-window combination the desktop calls light is not always the one
  somebody wants to look at.
- **The one exception is the tray's own autostart entry, and it is not
  configuration.** Whether the tray starts with the session is what the console
  knows about itself; the daemon's file has no way to describe it, and no mode
  definition and none of the backlight settings are reachable from that switch. The tray owns
  `$XDG_CONFIG_HOME/autostart/ombra-tray.desktop` and nothing else. The entry is
  the state — there is no settings file behind the switch — which is what makes
  "switched off and it stayed off" a property of the arrangement rather than of
  a mechanism. The package writes nothing into autostart: `dpkg -i` must not add
  a process to a session, for the same reason it does not take the thermal
  controls. The appearance choice sits beside it in the console's own settings
  file for the same reason: it is about the window rather than about the machine,
  and the daemon has no business holding it.
- **Every live setpoint is recorded in every telemetry sample** — active mode,
  power limits, frequency ceiling, turbo, EPP, HWP dynamic boost, platform
  profile. A log that does not say what was applied cannot be analysed. Mode and
  setpoint transitions are derived by diffing consecutive samples; do not add a
  separate event log for them.
- **Subjective verdicts are data too.** The tray can stamp the current sample
  with a user mark ("good", "too loud", "too slow"), so that a judgement made by
  ear is later recoverable together with the settings that produced it. Without
  this, live tuning produces an opinion that cannot be traced back to numbers.

## Metrics storage

- **Held in memory, flushed to disk infrequently.** Conserving NVMe endurance is
  deliberate; proposals to write every sample straight to disk are rejected.
- Retention limits are hard: the store does not grow, and eviction is a property
  of the format rather than of a background cleanup task.
- The format is chosen so that mode parameters can be tuned from it and charts
  drawn from it. Averaging that hides peaks is unacceptable.
- **A change of record layout converts what is already recorded; it does not
  discard it.** The samples are a week of a machine's life and several evenings
  of tuning, and the layout changes for reasons that have nothing to do with
  them. Conversion is a chain of one-step upgrades, each written once and then
  frozen, so a new version costs one struct and one function. Fields the old
  layout did not have are recorded as absent, never as zero.
- **A ring this build cannot read is kept, not deleted, and it never stops the
  daemon.** A layout from a newer build after a rollback, one that was never
  released, a header damaged by a power cut: all of them are moved aside with
  their bytes intact and replaced by a fresh ring. Telemetry is secondary to
  keeping the machine inside its envelope, so an unreadable ring costs the
  samples in it and nothing else.
- **One backup slot, replaced every time.** Rolling back from one version to
  the next and then to the next again would otherwise leave a ring behind at
  every step, on a disk whose endurance the rest of this section is built
  around, and the middle one is of no use to anybody. The layout is in the
  header, so the name does not have to carry it.
