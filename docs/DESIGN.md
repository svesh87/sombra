*English · [Русский](DESIGN.ru.md) · [Español](DESIGN.es.md)*

# How this is built, and why

Decisions about the shape of the project, with the reasoning that produced them.
They are not frozen. Each one is here so that changing it is an argument with a
reason rather than a discovery that somebody once thought about it — and so that
a change to one can be checked against the others.

What is frozen is the goal, and it is in `AGENTS.md`: a cool chassis outweighs
performance, noise has a calibrated ceiling, and the hardware safety rules are
not negotiable. Everything below serves that goal and is answerable to it.

Two things follow from that. A decision here changes by an ordinary edit,
together with the code, the documents and their translations. And a decision
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
  may move it either way, and a floor and a ceiling say what nothing goes below
  or above. Both bind the person as well, because a screen driven into unreadable
  — or into painful — is a failure and not a saving. The ceiling is the whole
  scale until somebody moves it, and it exists because a screen can be too bright
  to sit in front of, and because external panels differ in how much light they
  make of the same per cent.
- **The baseline can be set outright, not only derived.** Moving the brightness
  says what is wanted in the light of the moment and the baseline follows from it;
  that is the natural direction and it fails at exactly one thing — tuning for a
  light the room does not have right now. So there is a command and a control for
  the baseline itself, and the level follows from it instead. The baseline lives in
  the state file rather than the settings: it is what the person was last looking
  at, not a number they configured.
- **There are no brightness profiles.** One room, one set of settings: a
  response, a floor, a ceiling and a curve. A profile would be a name for a set of those
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
  changed lives in a text file the daemon writes, `/var/lib/sombra/settings.conf`,
  and nothing ships a file in `/etc`. The reason is not convenience: mode
  parameters are tuned by automated analysis of recorded telemetry, and the
  tooling that does the analysing has to be able to apply what it found —
  without root, and without operating a dialog.
- **Editing goes through the daemon and nowhere else.** `sombra config` and the
  tray send the same commands, values are validated by the same code that reads
  the file, and there is exactly one writer. The file is machine-written: hand
  comments in it do not survive a change, which is why the vocabulary is
  documented in `../README.md` instead. The selected mode and the other runtime
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
- **The panel icon is drawn rather than named.** Publishing a standard theme
  name would let the panel draw and recolour it, and it does not work: a
  StatusNotifierItem host prefers the name over the picture sent with it and
  draws a placeholder for a name its own theme cannot resolve, while this process
  can only ask its own icon theme, which follows a different inheritance and
  answers yes where the panel answers no. There is also no glyph to borrow — the
  themes here carry trios about volume, brightness, microphone and security, and
  none about power. The colours are fixed rather than palette colours, because
  the palette is the application's and the panel is the desktop's.
- **The panel icon carries its own plate.** It is the launcher icon's picture, a
  cloud over a sun, and a warm sun cannot be a flat glyph: measured against
  a light panel it comes to 1.5:1 where 3.5 is the floor, and the only single
  tone that clears both panels is a dark ochre, which does not read as a sun. On
  a plate the sun is seen against the plate, and the one contrast that involves
  the desktop is the plate's own, which clears 3.5:1 either way. The plate is
  flat where the launcher's is a gradient, whose dark end measures 1.3:1 against
  a dark bar and dissolves into it.
- **Three channels, and the guard is the plate.** Where the cloud sits says which
  mode is running, warm against grey says whether the mode is known at all, and
  the plate turns red under the emergency guard. Colouring the sun red instead
  would separate it from the blue plate by hue alone at 1.2:1 — the one cue that
  survives neither a small size nor a colour-blind eye. In the coolest envelope
  the sun is not drawn: rays coming out around the cloud would be a mode with the
  light still on, so there the second channel is the cloud's own colour. A daemon
  that is not answering and a reply with no mode in it are one picture, because
  the icon speaks about the mode and neither of them knows what the mode is;
  which of the two it is goes in the tooltip.
- **The cloud is held off the sun by a band of the plate's colour.** At 22 pixels
  a shared edge between a light cloud and a warm sun is a single blended pixel and
  the two stop reading as two objects. Eleven units of the 256-unit grid: below
  about seven the band is thinner than half a pixel at that size and disappears
  into the antialiasing, above about fourteen it reads as an outline drawn round a
  sticker.
- **A condition the picture cannot carry goes in the tooltip.** A control
  overwritten by another process and sensors that could not be read are both on
  the wire, and the window shows both as banners — but the window has to be opened
  first, and these are worth knowing about without opening anything. There is no
  fourth
  channel that survives 22 pixels, and a second alarm colour would be confused
  with the guard, which is the one state that changes what the machine does
  without being asked. So they are words in the tooltip, where there is room.
- **The console is translated; the command line is not.** The window follows the
  language of the system, from catalogues installed beside the binaries. The
  command line, the daemon's replies and the manual pages stay in English, because
  they are read in a terminal, quoted in documents and searched for by their exact
  wording — a translated `err` line is a line nobody can look up. The cost is
  visible in one place, a reply in English inside a translated window, and it is
  accepted rather than worked around.
- **An explanation belongs on the control it explains.** Paragraphs in the window
  cost the controls the height they needed and doubled what there was to
  translate, so what a control does is said in its tooltip. A test walks every
  control that sends a command and fails if one has nothing to say.
- **One line for the daemon's state and for its replies.** Two of them meant a
  strip of empty space under every tab until the first command. An accepted reply
  gives the line back after five seconds; a refusal holds it until the next
  command, because it answers something the person did and a few seconds is not
  long enough to be sure it was read.
- **The console's colours come from the desktop, and follow it while the window
  is open.** A desktop that configures Qt applications writes them a colour
  scheme; the console reads that file and reads it again whenever the appearance
  portal says something moved. This is not gold-plating: the desktop switches
  theme by itself at sunrise and sunset, a platform theme does not update the
  palette of a running application, and reopening a window does not restart the
  process — so without re-reading the file, a console open all day sits in
  yesterday's colours. Where no scheme is written, the portal's answer picks one
  of two palettes of ours, and the settings tab can override the choice outright.
- **The title bar is left alone.** It is Qt's own on this desktop, because the
  compositor offers to draw one and Qt 6.4 does not ask it to, and it takes its
  colours when the window is created. A change of scheme therefore reaches the
  widgets at once and the bar at the next opening. Rebuilding the window to
  refresh the bar was tried and took the process down with it, and one close and
  one click on the panel icon is a cheaper answer than that.
- **What is not attempted is imitating the desktop's widgets** — corner radii,
  container tints. That is a stylesheet chasing a moving target, and the colours
  are the part that makes a window look out of place.
- **The one exception is the tray's own autostart entry, and it is not
  configuration.** Whether the tray starts with the session is what the console
  knows about itself; the daemon's file has no way to describe it, and no mode
  definition and none of the backlight settings are reachable from that switch. The tray owns
  `$XDG_CONFIG_HOME/autostart/sombra-tray.desktop` and nothing else. The entry is
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
- **The record is 128 bytes with half of it spare, and changing that size is the
  risky edit.** A new field needs a version either way, because the bytes an old
  record has in that place are zero and what is wanted there is absence. What the
  spare room avoids is the size changing again — and the size is what makes "is
  this a ring" a question about the version in the header rather than about the
  record this build compiles. Answering it with `sizeof(Sample)` works for exactly
  as long as every version is the same size, and then sets aside a week of samples
  the day one is not.
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
