*English · [Русский](README.ru.md)*

# cpu_throttler

Thermal and power management for the ASUS Zenbook 14 UX3405MA (Intel Core Ultra
9 185H) on Linux: a background daemon that keeps the machine inside a chosen
thermal envelope, and a tray UI to switch between envelopes.

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
  be made from data rather than from guesswork.
- The daemon never changes display brightness.

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

## Status

Design stage. The working code so far is a set of shell tools:

| Script | Purpose |
|---|---|
| `scripts/cool-mode.sh` | manual stopgap: applies a fixed low-power state |
| `scripts/measure-combos.sh` | calibration sweep: measures how each control affects temperature and fan speed |
| `scripts/log-session.sh` | read-only logger: records what the machine does under a real workload |

## Documents

- `HARDWARE.md` — the control surface: which sysfs knobs exist, what they do,
  measured values, and which controls are absent on this platform.
- `AGENTS.md` (= `CLAUDE.md`) — repository conventions and quality gates.

Each document has a Russian copy beside it (`<NAME>.ru.md`). English is
canonical; where the two disagree, the English file is correct.

## License

MIT, see `LICENSE`.
