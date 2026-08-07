*English · [Русский](README.ru.md)*

# ombra

*Ombra* is Italian for shade — and in the shade a machine is both cooler and
dimmer, which is what this daemon is for.

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
- Envelopes are defined in a text configuration file, not in a settings dialog:
  they are tuned from recorded telemetry, and a text file is what tooling can
  edit. The daemon only ever reads that file.
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
- The tray is an experimentation console, not a settings editor. Durable mode
  definitions live in the configuration file; what the tray adds is sliders
  that apply immediately and are never written back, because choosing a
  setpoint is a judgement made by ear — the useful question is "does this
  sound acceptable right now", which cannot be answered by editing a file and
  restarting a daemon. It can also stamp that judgement onto the current
  sample, so an opinion stays traceable to the numbers that produced it.
- Selecting a mode never moves the display backlight. Bundling the backlight
  with the power profile is vendor behaviour this project treats as a defect.
- Following the ambient light sensor is a separate function with its own
  profile and its own switch. It works in a window around the level the person
  set rather than imposing one of its own, and a hand correction moves that
  window instead of being overridden. It is off until switched on, and that
  choice survives a reboot.

## Scope

Built and measured on one machine. Everything in `HARDWARE.md` was verified on:

| | |
|---|---|
| Laptop | ASUS Zenbook 14 UX3405MA |
| CPU | Intel Core Ultra 9 185H (Meteor Lake) |
| OS | Pop!_OS, kernel 7.0.11, COSMIC desktop |
| Drivers | `asus_wmi` / `asus_nb_wmi` / `asus_armoury`, ACPI `platform_profile` |

Other ASUS laptops expose a different set of controls; other desktops own
different D-Bus names. Porting means re-verifying the control surface first —
`HARDWARE.md` documents which controls exist here and, just as importantly,
which ones do not.

## Modes

Three, from the coolest envelope to the widest. The middle one is the working
mode: the machine spends the overwhelming majority of its time there, it is
applied at boot, and it is the one telemetry tuning targets.

| Mode | PL1/PL2 | Frequency ceiling | Turbo | EPP | Fan curve |
|---|---|---|---|---|---|
| `quiet` | 12/20 W | 40 % | off | `power` | `quiet` |
| `work` | 20/30 W | 50 % | off | `power` | `balanced` |
| `full` | 45/65 W | 100 % | on | `balance_performance` | `balanced` |

No mode touches the fan. Maximum fan speed is reachable only by explicit
command and through the emergency guard.

## What is in here

Three binaries, and the one you type is `ombra`.

| Binary | Purpose |
|---|---|
| `ombrad` | the daemon: applies the mode, follows the light sensor, records telemetry, answers on the D-Bus names |
| `ombra` | scriptable control: select a mode, tune live, stamp a verdict, read the telemetry ring |
| `ombra-tray` | the experimentation console |

`ombra export` is a subcommand rather than a program of its own, and it is the
only one that does not go through the daemon: the ring is a file, the daemon
keeps no history, and a week of samples has no business travelling through a
line protocol.

| Script | Purpose |
|---|---|
| `scripts/install.sh` | builds, installs and hands the thermal controls over |
| `scripts/rollback.sh` | gives them back to the vendor daemon |

The mode numbers that ship in `config/modes.conf` come from measurement on one
machine and are a starting point, not a result. They are meant to be tuned from
recorded telemetry, which is what `ombra export` and the subjective marks are
for.

## Building and installing

Requires CMake 3.16, a C++17 compiler and Qt 6 (Core, Network, DBus, Widgets,
Test). Building the package additionally needs `debhelper` and `dpkg-dev`.

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
sudo dpkg -i build-package/ombra_*.deb
sudo systemctl disable --now com.system76.PowerDaemon.service
sudo systemctl mask com.system76.PowerDaemon.service
sudo systemctl enable --now ombrad
sudo usermod -aG ombra "$USER"    # log out and back in, for the tray
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
ombra status                   # readings and setpoints as JSON
ombra mode quiet
ombra set pl1=15 max_perf_pct=40   # live, immediate, never persisted
ombra mark too-loud            # a verdict, stamped onto the sample
ombra fan max                  # explicit demand only
ombra export --format csv > session.csv
```

Durable mode definitions live in `/etc/ombra/modes.conf`, which the
daemon reads and never writes. Changes take effect without a restart.

## Brightness that follows the room

A separate function from the thermal mode, with its own profile and its own
switch. Selecting a mode never moves the backlight, and nothing about the
backlight reaches the thermal side.

```bash
ombra brightness on
ombra brightness off           # remembered across a reboot
ombra brightness profile indoors
```

The model is short:

- **The level you set is the baseline.** Moving the brightness is a statement
  about what you want in this light, so it becomes the new baseline and the
  automation's window moves with it. It is never overridden.
- **The profile says how far the automation may wander from that baseline**, as
  a span measured in both directions: a span of 10 around a baseline of 80 %
  means 70–90 %. A floor and a ceiling then bound it absolutely, cutting the
  window rather than moving it — a baseline of 90 % with a span of 10 and a
  ceiling of 95 gives 80–95 %. They constrain the automation, never you: a level
  you set outside them is accepted.
- **The light curve chooses a point inside that window**, not an absolute level,
  so narrowing the span makes the automation move less rather than pinning the
  backlight to an edge.
- **It does not hunt.** The target is snapped to coarse steps, each boundary
  carries a hysteresis margin, a new step has to hold for several seconds before
  anything happens, and the move itself is a fade rather than a jump. Steady
  light produces no writes at all.
- **Off is off.** Switched off, the daemon does not write the backlight at all,
  and neither switching it on nor off moves the backlight by itself.

The curve is defined in `/etc/ombra/modes.conf` in the sensor's own
units, which are not lux — see `HARDWARE.md`. The `light` column of an exported
session is what to calibrate it against.

## What survives a reboot

| Kept | Why |
|---|---|
| the selected mode | choosing one is a deliberate act |
| the brightness automation on/off | something you switched off must not come back on its own |
| the brightness profile and your baseline level | otherwise it would have to be set again after every boot |
| the charge threshold | it lives in sysfs, is lost on reboot, and nothing else re-applies it |

Deliberately not kept: live tuning, which applies immediately and is never
written back anywhere, and the forced maximum fan speed — 6100 RPM surviving a
reboot is a surprise, not restored state.

These live in a state file separate from the configuration, so the maintainer's
comments in the configuration survive and the daemon never rewrites it.

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
sudo systemctl disable --now ombrad
sudo systemctl unmask com.system76.PowerDaemon.service
sudo systemctl enable --now com.system76.PowerDaemon.service
```

The daemon restores automatic fan control and a confirmed-safe power state on
every exit path, so stopping it never leaves the machine in a state it cannot
cool itself out of. The rollback script checks that the fan really did go back
to automatic rather than assuming it.

## Documents

- `HARDWARE.md` — the control surface: which sysfs knobs exist, what they do,
  measured values, and which controls are absent on this platform.
- `AGENTS.md` (= `CLAUDE.md`) — repository conventions, frozen design
  decisions and quality gates.
- `config/modes.conf` — the mode definitions, commented in place. This is the
  reference for what each setting means.

Each document has a Russian copy beside it (`<NAME>.ru.md`). English is
canonical; where the two disagree, the English file is correct.

## License

MIT, see `LICENSE`.
