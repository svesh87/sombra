*English · [Русский](docs/README.ru.md) · [Español](docs/README.es.md)*

# sombra

*Sombra* is Spanish for shade, and in the shade a machine is both cooler and
dimmer. That is what this daemon is for.

Thermal, noise and backlight management for the ASUS Zenbook 14 UX3405MA (Intel
Core Ultra 9 185H) on Linux: a background daemon that keeps the machine inside a
chosen thermal envelope and follows the ambient light sensor, and a tray console
to switch envelopes and tune them by ear.

## Why this exists

Three properties of this platform combine badly:

- **The firmware exposes no fan control.** `pwm1_enable` accepts only *automatic*
  and *full speed*; there is no manual PWM value and no fan curve — not in
  hwmon, not in `asus-armoury`. Fan speed is therefore not something software
  can set. It is only something software can *cause*, by changing how much heat
  the package produces.
- **The firmware's performance profile removes the power limit.** It programs
  RAPL PL1 to 200 W on a laptop rated for roughly 28 W — more than the chassis
  can dissipate, and enough to reach hardware thermal shutdown before the
  operating system can react.
- **The vendor tooling bundles knobs that need to be separate.** `system76-power`
  ships three fixed profiles; each one sets the CPU frequency ceiling, turbo,
  the energy-performance preference, the ACPI platform profile and the display
  backlight together. The combination most useful here — a hard power cap with
  a fan curve that is *not* lazy — cannot be expressed. Neither can "cap the
  CPU but leave my screen brightness alone".

The result is a machine that is either hot and quiet or cool and loud, with no
way to ask for the point in between. This project provides that point.

## Design summary

- A single daemon owns every thermal and power control. Two processes writing
  the same sysfs nodes produce race-dependent behaviour, not redundancy, so
  `system76-power` is masked rather than layered on top.
- The daemon answers on the standard `net.hadess.PowerProfiles` and
  `org.freedesktop.UPower.PowerProfiles` D-Bus names, and on
  `com.system76.PowerDaemon` for compatibility with the COSMIC panel applet.
  Both the desktop panel and the tray therefore keep working.
- Envelopes are edited through the daemon, from the command line or the tray,
  and the daemon keeps them in a text file of its own. They are tuned by
  analysing recorded telemetry, so the tooling that does the analysing has to be
  able to apply what it found — without root, and without operating a dialog.
- The daemon records sensor telemetry once per second into a fixed-size ring
  held in memory and flushed to disk infrequently, so that tuning decisions can
  be made from data rather than from guesswork. Every sample carries the
  setpoints that were in force, because a log that does not say what was
  applied cannot be analysed.
- Setpoints are static per mode. There is no feedback controller: measurement
  showed static values handle both idle and sustained load, and a controller
  would add coefficients to tune and failure modes — hunting and sticking —
  that static values cannot have.
- An emergency thermal guard is always active regardless of mode. It only ever
  lowers, and it is the one thing that will drive the fan by itself. This is a
  safety interlock, not regulation.
- The tray is a console, not a settings dialog. Pick a mode and its controls
  take the mode's numbers; move one and it applies at once, and the mode
  selector empties, because what is running is then not one of the three. One
  button writes the controls into a mode you name, through the daemon and on the
  same commands the command line sends. Three more stamp a judgement — good, too
  loud, too slow — onto the current sample, so an opinion made by ear stays
  traceable to the numbers that produced it.
- Selecting a mode never moves the display backlight. Bundling the backlight
  with the power profile is vendor behaviour this project treats as a defect.
- Following the ambient light sensor is a separate function with its own
  switch. It works around the level the person set rather than imposing one of
  its own: the room adds or subtracts from that level, and a hand correction is
  a new statement of what is wanted rather than something to override. It is off
  until switched on, and that choice survives a reboot.

## Scope

Built and measured on one machine. Everything in `docs/HARDWARE.md` was verified on:

| | |
|---|---|
| Laptop | ASUS Zenbook 14 UX3405MA |
| CPU | Intel Core Ultra 9 185H (Meteor Lake) |
| OS | Pop!_OS, kernel 7.0.11, COSMIC desktop |
| Drivers | `asus_wmi` / `asus_nb_wmi` / `asus_armoury`, ACPI `platform_profile` |

Other ASUS laptops expose a different set of controls; other desktops own
different D-Bus names. Porting means re-verifying the control surface first —
`docs/HARDWARE.md` documents which controls exist here and, just as importantly,
which ones do not.

## Modes

Three, from the coolest envelope to the widest. The middle one is the working
mode: the machine spends the overwhelming majority of its time there, it is
applied at boot, and it is the one telemetry tuning targets.

| Mode | PL1/PL2 | Frequency ceiling | iGPU ceiling | Turbo | EPP | Fan |
|---|---|---|---|---|---|---|
| `quiet` | 12/20 W | 40 % | 1000 MHz | off | `power` | `quiet` curve |
| `work` | 20/30 W | 50 % | 2350 MHz | off | `power` | `balanced` curve |
| `full` | 45/65 W | 100 % | 2350 MHz | on | `balance_performance` | `balanced` curve |

The fan is a setpoint like the others: a mode names one of the firmware's three
curves, and it may ask for full speed. What it must not do is arrive there by
itself — nothing runs the fan up without having been told to, and the emergency
guard is the one exception.

## What is in here

Three binaries, and the one you type is `sombra`.

| Binary | Purpose |
|---|---|
| `sombrad` | the daemon: applies the mode, follows the light sensor, records telemetry, answers on the D-Bus names |
| `sombra` | scriptable control: select a mode, tune live, stamp a verdict, read the telemetry ring |
| `sombra-tray` | the experimentation console |

`sombra export` is a subcommand rather than a program of its own, and it is the
only one that does not go through the daemon: the ring is a file, the daemon
keeps no history, and a week of samples has no business travelling through a
line protocol.

| Script | Purpose |
|---|---|
| `scripts/install.sh` | builds, installs and hands the thermal controls over |
| `scripts/rollback.sh` | gives them back to the vendor daemon |
| `scripts/screenshots.sh` | takes the documentation pictures |

The console has a `?` at the end of its status line. It opens the reference: every
control of every tab with its units, its range and what it costs, plus the model the
window is built on, in the language the window is speaking. Installed as
`/usr/share/sombra/help/sombra-help.<language>.html`.

The package installs manual pages — `sombra(1)`, `sombrad(8)`, `sombra-tray(1)` —
and shell completion for bash, zsh and fish. Completion lists the verbs from a
static file and asks the daemon only for mode names, because a Tab that opens a
socket is a Tab that hangs when the daemon is not there.

The mode numbers compiled into the daemon come from measurement on one machine
and are a starting point, not a result. They are meant to be tuned from
recorded telemetry, which is what `sombra export` and the subjective marks are
for.

External monitors are driven over DDC/CI, which is a conversation on the
connector's I2C bus rather than a sysfs node — about 300 ms to write and 350 ms to
read, measured on three monitors. `sombra monitor list` says which ones answer;
`sombra monitor level <percent> [<id>]` sets one, or every one that answers. The
reply means the request was sent: the level in `status` follows the read that
confirms it, and that read is also how a hand on the monitor's own buttons is
noticed. The internal panel is not among them and cannot be — a laptop panel does
not answer DDC/CI. Each monitor's level, and one byte saying which monitor it is,
goes into every sample in the slot of its socket.

How much telemetry is kept is a setting, `retention_days` — one day to thirty,
seven by default. The depth is the ring's own size rather than a policy applied to
it, so changing it rebuilds the file and carries the samples across, and a shorter
retention drops the oldest. At 128 bytes a record that is about 10.5 MiB a day, and
the file occupies its whole size from the moment it is made. `enabled=off` stops the
appending and nothing else: the sample is still taken, `status` still answers, and
the file keeps what it has. Both are on the console's *Settings* tab as well.

A release that changes the record layout converts the ring at the next start,
field by field, and records what the old format did not have as absent rather
than as zero. A file this build cannot read is kept as `metrics.ring.bak` and a fresh ring
takes its place: a layout from a newer build after a rollback, one that was
never released, or a header damaged by a power cut. Nothing is deleted for being
unreadable, and an unreadable ring costs the samples in it and nothing else —
the daemon carries on managing the machine.

There is one backup slot and each rollback replaces it. Rolling back twice
would otherwise leave a ring behind at every step, and the middle one is of no
use to anybody.

`scripts/install.sh` also exports the ring to CSV before it replaces anything,
into `$XDG_STATE_HOME/sombra/telemetry` (`--dump-dir` overrides). CSV outlives
any layout, which the conversion above cannot promise for a format nobody has
written yet.

## Building and installing

Requires CMake 3.16, a C++17 compiler and Qt 6 (Core, Network, DBus, Widgets,
Test, LinguistTools). On Debian and its derivatives that is:

```bash
sudo apt install build-essential cmake \
    qt6-base-dev qt6-base-dev-tools libqt6svg6 \
    qt6-l10n-tools qt6-tools-dev \
    debhelper dpkg-dev lcov
```

`qt6-l10n-tools` and `qt6-tools-dev` are the translation tools — `lupdate`,
`lrelease` and the CMake package that drives them. `debhelper` and `dpkg-dev` are
for `make package`, and `lcov` for the coverage gate; building and running the
binaries needs neither.

```bash
make            # build
make test       # the full suite; no test touches real hardware
make gates      # lint, tests, the coverage threshold and git diff --check
make package    # builds the .deb and checks what came out of it
```

One script does the whole rollout — build, install, hand the controls over, add
you to the group. It builds as you and elevates only for the parts that need
root, and it refuses to run on a machine that is not the model this was
verified on:

```bash
./scripts/install.sh
```

Run it again after any change: every step is idempotent and it restarts the
daemon rather than only enabling it, so it is the upgrade path as well. A tray
you already have open keeps running the previous build; the script says so.

Installing the package by itself starts **nothing**. There can be exactly one
owner of these controls, so handing them over is a separate decision, and
`dpkg -i` must not make it. What the script does, if you would rather do it by
hand:

```bash
sudo dpkg -i build-package/sombra_*.deb
sudo systemctl disable --now com.system76.PowerDaemon.service
sudo systemctl mask com.system76.PowerDaemon.service
sudo systemctl enable --now sombrad
sudo usermod -aG sombra "$USER"    # log out and back in, for the tray
```

A group you were added to while logged in only takes effect after a fresh
login, which is the usual reason the tray says the daemon is not answering. The
daemon hands its socket to that group itself, right after binding it — not from
the unit file, where it would race its own bind and fail silently.

`com.system76.PowerDaemon.service` is the vendor unit's real name.
`system76-power.service` is only an `Alias=` of it, and masking the alias
leaves the real unit enabled — it would come back at the next boot and there
would be two owners of one control surface. Masking also does not stop a
running unit, which is why it is disabled first.

`make install` remains available for a build without a package. It installs the
same files, and the commands above are still what puts the daemon in charge.

## Using it

The desktop panel's three slots select this project's modes. Everything else
goes through the socket:

```bash
sombra status                   # readings and setpoints as JSON
sombra mode quiet
sombra set pl1=15 max_perf_pct=40   # live, immediate, not yet saved
sombra mark too-loud            # a verdict, stamped onto the sample
sombra fan max                  # explicit demand only
sombra export --format csv > session.csv
```

Live tuning is not durable, and that is the point of it. When a value turns out
to be worth keeping, one command writes it into a mode:

```bash
sombra config show                              # what is saved, as JSON
sombra config set mode quiet pl1_w=10 pl2_w=16  # a durable definition
sombra config save live to quiet                # keep what the sliders found
sombra config default_mode work                 # the mode applied at boot
sombra config reset modes                       # back to the built-in values
```

### Where settings live

The daemon carries its defaults compiled in and writes what it is told into
`/var/lib/sombra/settings.conf`. There is no file in `/etc`: editing goes
through the daemon, so that a script analysing telemetry can apply what it
found without root and without a text editor, and so that the tray and the
command line cannot drift apart. The file appears the first time something is
changed; delete it and the built-in values are back.

It is written by the daemon and comments in it do not survive a change. What
each key means is below rather than in the file for that reason.

| Key | Unit and range | Meaning |
|---|---|---|
| `label` | text | what the panel and the tray show |
| `platform_profile` | `quiet`, `balanced`, `performance` | selects the EC fan curve, which is the noise-versus-temperature dial |
| `pl1_w` | watts, 1–45 | RAPL long-term limit; the most effective control under sustained load |
| `pl2_w` | watts, `pl1_w`–65 | RAPL short-term limit |
| `max_perf_pct` | percent, 8–100 | `intel_pstate` frequency ceiling |
| `turbo` | `on`, `off` | |
| `epp` | `performance`, `balance_performance`, `balance_power`, `power` | energy preference; with `hwp_dynamic_boost` this is what suppresses the short bursts that heat the package while the machine is nominally idle |
| `hwp_dynamic_boost` | `0`, `1` | |
| `gt_max_mhz` | MHz, 800–2350 | iGPU ceiling. 800 is `gt_RPn_freq_mhz`, below which the driver refuses the write outright |
| `fan` | `auto`, `max` | `auto` leaves the curve named by `platform_profile` in charge; `max` is roughly 6100 RPM |

The ceilings on `pl1_w` and `pl2_w` are the highest values confirmed on this
machine. Nothing raises a limit past them, and a larger number is clamped and
reported rather than accepted. The kernel also lists `default` for `epp`, which
this daemon refuses: the node takes it and then reports the preference it
actually chose, so it reads back as something else and looks like another
process writing the control.

Full speed is loud enough that nothing chooses it quietly: a mode has to say
`fan=max`, or somebody has to type `sombra fan max` or pick it in the console.
The emergency thermal guard is the exception, and it is a safety interlock
rather than automatic control.

There are exactly three modes and they are edited in place, because the desktop
panel has three slots and a fourth definition would live where the panel cannot
reach it.

The console speaks the language of the system, in Russian, English or Spanish.
Only the console: `sombra --help`, the daemon's replies and the manual pages stay
in English, because they are read in a terminal, quoted in documentation and
searched for by their exact wording. That shows in one place — a reply on the
status line is English inside a translated window — and it is the honest trade
rather than an oversight. The catalogues are in
`/usr/share/sombra/translations`, and an unknown language shows the source
language, which is English.

The console's *Settings* tab holds what is about the program rather than the
machine: whether it starts with the session, what it looks like, and one button
that puts every mode definition and every backlight setting back to the values
compiled into the daemon.

Starting with the session is an XDG autostart entry and nothing else —
installing the package adds no process to your session, and the entry is the
whole state, so something switched off stays off. It takes effect at the next
login. The appearance is light, dark, or whatever the desktop says; it is the
console's own setting and does not reach the panel icon, which is drawn from
fixed colours: the palette belongs to the application and the panel is coloured
by the desktop, and the two disagree exactly when the desktop is light with a
dark bar.

The icon itself is a cloud over a sun, and it answers three things at a glance.
Where the cloud sits says which mode is running — the whole of the picture for
the coolest envelope, half the sun in between, gone for the widest. Warm against
grey says whether the mode is known at all, and in the coolest envelope, where
there is no sun to colour, that falls to the cloud. A daemon that is not
answering and a reply with no mode in it are the same grey picture: the icon
speaks about the mode, and neither of them knows what the mode is. The plate
turns red while the emergency guard is clamping. The two conditions the picture
cannot carry, a control overwritten by another process and sensors that could not
be read, are named in the tooltip along with the mode, the package temperature
and the fan speed.

There is one console per session: started while another is running, it asks that
one to open its window and exits. Two of them would mean two icons in the panel
and two pollers on the daemon, and no way to tell which icon belongs to which.

Started with the session, the tray waits up to a minute for a panel to appear
before deciding there is none: the tray host registers over the session bus and
is routinely a second or two behind an autostarted process. Until an icon
exists, closing the console ends the process — otherwise it would live on with
no window and no icon, killable only by name.

## Brightness that follows the room

A separate function from the thermal mode, with its own switch. Selecting a mode
never moves the backlight, and nothing about the backlight reaches the thermal
side.

```bash
sombra brightness on
sombra brightness off             # remembered across a reboot
sombra brightness level 80        # the same as moving the control yourself
sombra brightness base 72         # the level the automation works around
sombra brightness set response_pct=35 floor_pct=20 ceiling_pct=90
sombra config set brightness response_pct=35 floor_pct=20   # written down
```

The model in one line: **the light adds to or subtracts from the level you
set.**

- **The level you set is the baseline.** Moving the brightness says what you
  want in this light. The daemon takes the room's contribution of that moment
  back out of it, and what is left is the baseline — which is what makes
  returning to the same light return to the same brightness, exactly. The
  baseline can also be set outright, with `brightness base` or the slider beside
  the brightness one, which is what to reach for when the light in the room is
  not the light you are tuning for.
- **The reaction says how far the room may move it**, in percentage points and
  the same amount either way. A reaction of 30 around a baseline of 80 % means
  a dark room can pull it to 50 % and a bright one push it to 100 %.
- **The floor is the level nothing goes below** — not the automation, and not a
  slip of the hand either. **The ceiling is the same bound from above**, the whole
  scale until you move it, and it is what to reach for on a screen that is
  unpleasant at its own maximum.
- **It does not hunt.** The target is snapped to coarse steps, each boundary
  carries a hysteresis margin, a new step has to hold for several seconds before
  anything happens, and the move itself is a fade rather than a jump. Steady
  light produces no writes at all.
- **Off is off.** Switched off, the daemon does not write the backlight at all,
  and neither switching it on nor off moves the backlight by itself.

The level, the baseline, the reaction, the floor and the ceiling are five
sliders in the console, all in per cent, because "is this right" is a judgement made by eye in
the room it is meant for. The sensor reading is not on screen: what the room
measures is the daemon's business.

The one setting that is not a slider is the curve, which maps a light reading in
lux to a position from 0 to 100: 0 subtracts the whole reaction, 50 contributes
nothing, 100 adds the whole reaction. Its shape is calibrated from recorded
telemetry rather than judged on the spot, and 50 is not a free choice — it
belongs at the light the room usually has. The `light_lux`, `brightness` and
`brightness_base` columns of an exported session are what to calibrate it
against.

## What survives a reboot

| Kept | Why |
|---|---|
| the selected mode | choosing one is a deliberate act |
| the brightness automation on/off | something you switched off must not come back on its own |
| your baseline brightness | otherwise it would have to be set again after every boot |
| the charge threshold | it lives in sysfs, is lost on reboot, and nothing else re-applies it |
| whether the tray starts with the session | the autostart entry is the state, so switched off stays off |
| the mode and backlight settings | they are what tuning produces, and losing them would mean tuning again |

Deliberately not kept: live tuning, which applies immediately and is saved only
when told to. Full fan speed is part of that — 6100 RPM surviving a reboot is a
surprise, not restored state.

The definitions and the runtime choices are two files, `settings.conf` and
`state`, and they are separate because they have different lifetimes: one is
edited when numbers are tuned, the other is rewritten whenever somebody picks
something. The autostart entry is the tray's own and the daemon knows nothing
about it.

## Rolling back

If the modes here turn out worse than the vendor's, no code is involved:

```bash
./scripts/rollback.sh              # hand the controls back
./scripts/rollback.sh --remove     # ... and uninstall, keeping the telemetry
./scripts/rollback.sh --purge      # ... and delete the recorded telemetry too
```

Handing back is the default and it leaves the package and the recorded
telemetry alone: the telemetry is the material the modes are tuned from, and a
bad afternoon is not a reason to throw it away. By hand, that is:

```bash
sudo systemctl disable --now sombrad
sudo systemctl unmask com.system76.PowerDaemon.service
sudo systemctl enable --now com.system76.PowerDaemon.service
```

The daemon restores automatic fan control and a confirmed-safe power state on
every exit path, so stopping it never leaves the machine in a state it cannot
cool itself out of. The rollback script checks that the fan really did go back
to automatic rather than assuming it.

## Documents

- `docs/HARDWARE.md` — the control surface: which sysfs knobs exist, what they do,
  measured values, and which controls are absent on this platform.
- `docs/SCREENSHOTS.md` — what the tray menu and the console look like, and the one
  command that makes the pictures again.
- `docs/DESIGN.md` — how the project is built and why: the decisions about its
  shape, with the reasoning that produced them.
- `AGENTS.md` (= `CLAUDE.md`) — repository conventions, the goal the project is
  answerable to, and the quality gates.

Each document has a Russian copy beside it (`<NAME>.ru.md`). English is
canonical; where the two disagree, the English file is correct.

## License

MIT, see `LICENSE`.
