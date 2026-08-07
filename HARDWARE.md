*English · [Русский](HARDWARE.ru.md)*

# Control surface

What can and cannot be controlled on this platform, and what the controls
actually do. Every value here was read from the running machine; anything not
verified is marked as such.

Platform: ASUS Zenbook 14 UX3405MA, Intel Core Ultra 9 185H (22 threads,
4.8 GHz max), Pop!_OS with kernel 7.0.11 and the COSMIC desktop. Drivers
`asus_wmi` / `asus_nb_wmi` / `asus_armoury`; ACPI `platform_profile` functional.
`system76-power` and `thermald` both run by default; `power-profiles-daemon` is
not installed.

## What is not available

Establishing this first saves re-deriving it later.

- **No manual fan control.** `pwm1_enable` accepts `2` (automatic, EC-driven)
  and `0` (full speed) only. There is no PWM value node and no fan curve
  anywhere: `/sys/class/firmware-attributes/asus-armoury/attributes/` contains
  a single entry, `pending_reboot` — this firmware exposes neither fan curves
  nor PPT attributes. Fan speed can only be influenced indirectly, by changing
  the amount of heat produced or by selecting a different EC curve through
  `platform_profile`.
- **No undervolting.** Blocked by Intel on Meteor Lake.
- **`asusctl` untested.** Not installed here; UX3405MA support unverified.

## Thermal policy profile

- `/sys/firmware/acpi/platform_profile` — `quiet` / `balanced` / `performance`;
  confirm the set through `platform_profile_choices`.
- `/sys/devices/platform/asus-nb-wmi/throttle_thermal_policy` — 0 = balanced,
  1 = performance, 2 = quiet. Tracks `platform_profile`.
- `system76-power profile balanced|performance` switches both without root. It
  has no `quiet` setting; that requires writing sysfs directly as root.

This selects the EC fan curve and is the only way to shift the curve as a whole.
`quiet` makes the EC tolerate more heat before ramping, which trades a hotter
chassis for a slower fan.

**Writing `platform_profile` makes the firmware reassert its own power limits.**
Any RAPL value must therefore be written *after* the profile, not before, and
re-checked afterwards.

## Fan

- hwmon directory: `/sys/devices/platform/asus-nb-wmi/hwmon/hwmon*/`. The
  `hwmon*` index is not stable across boots — resolve by `name` (`asus`).
- `fan1_input` — speed in RPM.
- `pwm1_enable` — `2` automatic, `0` full speed. Root required.
- A second speed reading is exposed by the `acpi_fan` hwmon (`fan1`) and serves
  as a cross-check.

Measured behaviour, 12-minute sample at 0.2 Hz with load average around 0.6:

| | |
|---|---|
| Idle speed | 1900 RPM in 109 of 144 samples |
| Range observed | 1900–2500 RPM idle; 3200 under light load; 6100 maximum |
| Apparent floor | **1900 RPM** — never observed lower, never observed stopped |
| Ramp threshold | above the floor at a package temperature around 62 °C |

The floor matters: at idle there is no fan speed left to give up, so idle
temperature cannot be improved by making the fan quieter — only by producing
less heat or by forcing more airflow.

## CPU power and frequency

- `/sys/devices/system/cpu/intel_pstate/no_turbo` — 1 disables turbo.
- `/sys/devices/system/cpu/intel_pstate/max_perf_pct` — frequency ceiling in
  percent (`min_perf_pct` is 8 here).
- `/sys/devices/system/cpu/intel_pstate/hwp_dynamic_boost` — 1 by default.
- `/sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference` —
  `default` / `performance` / `balance_performance` / `balance_power` / `power`.
  Defaults to `balance_performance`.
- `scaling_driver` is `intel_pstate`, `scaling_governor` is `powersave`.

Together, the default EPP and HWP dynamic boost let the package boost to near
maximum frequency for trivial work. These short bursts are what heats the
package when the machine is nominally idle; the sustained power limit is not
reached at idle and therefore does not affect it.

### RAPL

`/sys/class/powercap/intel-rapl:0/` (`name` = `package-0`):

| Node | Meaning | Observed |
|---|---|---|
| `constraint_0_power_limit_uw` | PL1, long term, µW | 200 W as set by the firmware's performance profile |
| `constraint_1_power_limit_uw` | PL2, short term, µW | 115 W |
| `constraint_2_power_limit_uw` | peak power | 217 W |
| `constraint_0_time_window_us` | PL1 averaging window | 31 981 568 µs (≈32 s) |
| `constraint_1_time_window_us` | PL2 averaging window | 2440 µs |
| `energy_uj` | package energy counter, root only | wraps at `max_energy_range_uj` |

Sub-domains `intel-rapl:0:0` (`core`) and `intel-rapl:0:1` (`uncore`) carry
separate energy counters, which distinguishes CPU heat from iGPU heat.

PL1 is the most effective control for sustained load. The PL1 time window is
writable and underused: a short window produces steady power instead of sprints,
which lowers peak temperature at nearly no perceptible cost.

**A 200 W PL1 on a laptop rated for roughly 28 W is more than the chassis can
dissipate.** Under sustained load the package can reach the hardware cutout
before the throttling loop responds; the kernel log then ends mid-line with no
shutdown record, which is the signature of a THERMTRIP rather than a software
fault. This is the failure mode the project exists to prevent.

## Integrated GPU

Driver `i915`, exposed as `card1`. A video call loads the graphics engine as
heavily as the cores — decoding every remote stream, encoding the local one, and
any continuous inference such as auto-framing — and **none of the CPU-side
controls affect that.** `max_perf_pct`, `no_turbo` and EPP act on the cores
only. Package PL1 does constrain the total, but the graphics engine also has
controls of its own:

| Node | Meaning | Observed |
|---|---|---|
| `/sys/class/drm/card*/gt_max_freq_mhz` | frequency ceiling, writable | 2350 |
| `/sys/class/drm/card*/gt_min_freq_mhz` | frequency floor | 100 |
| `/sys/class/drm/card*/gt_act_freq_mhz` | current frequency | — |
| `/sys/class/drm/card*/gt_RP0_freq_mhz` | hardware maximum | 2350 |
| `/sys/class/powercap/intel-rapl:0:1/constraint_0_power_limit_uw` | power limit for the uncore domain | — |

Together these allow the graphics engine to be limited without touching CPU
performance, which matters when the heat originates there.

The Intel media driver (`iHD_drv_video.so`) is installed, so hardware video
decode and encode are available at the system level. Whether an individual
application uses them is a separate question: software decoding of many
simultaneous video streams is expensive and entirely avoidable.

### Hardware throttle counters

`/sys/devices/system/cpu/cpu*/thermal_throttle/` — `core_throttle_count`,
`package_throttle_count`, `*_total_time_ms`, `*_max_time_ms`. These count the
hardware's own emergency throttling: a rising counter is a direct "the software
envelope failed" signal. The counters are cumulative per boot, so only their
rate of change is meaningful.

## Battery

`/sys/class/power_supply/BAT0/charge_control_end_threshold` exists and is
writable, so charge limiting is a single-file operation.

## Sensors

| hwmon `name` | Provides |
|---|---|
| `coretemp` | `Package id 0` and per-core temperatures; high = crit = 110 °C |
| `acpitz` | chassis / board temperature |
| `nvme` | Composite plus two sensors; crit 84.8 °C |
| `iwlwifi_1` | Wi-Fi module temperature |
| `BAT0` | battery power |
| `asus` | fan speed |
| `acpi_fan` | fan speed, second source |

`hwmon*` indices are not stable across boots. Resolve every sensor by `name`.

## Ambient light sensor and backlight

Both exist and are independent of everything above. They are documented here
because the daemon writes the backlight, not because either has anything to do
with heat: selecting a thermal mode never moves the backlight.

| Node | Value |
|---|---|
| `/sys/bus/iio/devices/iio:device0/name` | `als` — for identification only, see below |
| `.../in_illuminance_raw` | rises with light; readable without root |
| `.../in_illuminance_scale` | `0.001` |
| `.../in_illuminance_sampling_frequency` | `10.0` |
| `/sys/class/backlight/intel_backlight/max_brightness` | 400 |
| `/sys/class/backlight/intel_backlight/brightness` | `root:root 0644` |

Two properties decide how the daemon uses them.

**The illuminance is not calibrated lux.** `in_illuminance_raw` multiplied by
the scale lands roughly two orders of magnitude below what a light meter reports
for the same room — the sensor sits behind dark bezel glass and nothing corrects
for it. The reading is therefore used, recorded and configured in the sensor's
own units, and the response curve is calibrated against them from recorded
telemetry. Calling the number lux would be a claim of accuracy that is not
there.

**The backlight is writable only by root.** So the daemon owns it and the tray
cannot touch it: everything the tray does to the backlight goes through the
daemon. Anything else running as root can write it too, which is why the daemon
remembers the last level it knows about — a reading that differs from that
memory is somebody else's write, and in practice that somebody is the person on
the brightness keys.

`iio-sensor-proxy` is installed but not running, so nothing else currently
drives the backlight from the sensor. If it were started, it and this daemon
would be two owners of one control, which is the arrangement this project
exists to avoid.

The iio device index is no more stable than a `hwmon` one. The daemon locates
the sensor by the presence of an illuminance channel rather than by index or by
`name`, because drivers disagree about what to put in `name` and a device
exposing an illuminance channel is a light sensor whatever it calls itself.

## Measured thermal behaviour

Measured on one machine. Absolute values depend on the condition of an
individual unit's cooling; the relationships between settings are the part that
transfers.

- Package temperature settles at **60–63 °C at idle** with the fan at its floor
  and load average below 1.
- Switching from the vendor `balanced` profile to `battery` — frequency ceiling
  cut from 100 % to 50 %, turbo disabled, EC curve moved to `quiet` — changed
  package temperature by roughly one degree and left fan speed unchanged. Two
  effects cancel: less heat is produced, and the lazier curve removes exactly
  as much airflow. Suppressing bursts alone therefore buys nothing.
- The vendor profiles do not touch RAPL at all. Power limits written by hand
  survive every profile switch untouched.

## Ownership conflicts

- `system76-power` owns all three D-Bus names at once: `com.system76.PowerDaemon`,
  `net.hadess.PowerProfiles` and `org.freedesktop.UPower.PowerProfiles`.
  `power-profiles-daemon` is not installed, so nothing else competes for the
  latter two. The stock policy file already permits any root process to own
  them, and none of the three is bus-activatable — there is no activation file
  under `/usr/share/dbus-1/system-services/` — so masking the unit transfers
  ownership cleanly and nothing restarts the vendor daemon on demand.
- **The vendor unit's real name is `com.system76.PowerDaemon.service`**;
  `system76-power.service` is an `Alias=` declared by that unit, and the alias
  exists on disk only as a symlink to it. Masking the alias replaces that
  symlink and leaves the real unit enabled in `multi-user.target.wants`, so the
  vendor daemon returns at the next boot. Everything that masks, unmasks or
  conflicts with it must name the real unit.
- `system76-power` also sets the display backlight on profile switch.
- `thermald` runs by default and writes to RAPL and `intel_pstate` on its own.
- `system76-scheduler` adjusts process priorities and is unrelated to thermals.

All sysfs writes are lost on reboot.

### The vendor D-Bus interface

Taking the name over means answering the whole of it. As introspected on this
machine, `com.system76.PowerDaemon` is sixteen methods and two signals:

| Member | Signature | Note |
|---|---|---|
| `Battery` / `Balanced` / `Performance` | `() → ()` | select a profile |
| `GetProfile` | `() → s` | |
| `GetChargeThresholds` | `() → (y, y)` | start and end per cent |
| `SetChargeThresholds` | `((yy)) → ()` | |
| `GetChargeProfiles` | `() → a(sssyy)` | id, title, description, start, end |
| `GetGraphics` / `SetGraphics` | `() → s` / `(s) → ()` | |
| `GetGraphicsPower` / `SetGraphicsPower` | `() → b` / `(b) → ()` | |
| `AutoGraphicsPower` | `() → ()` | |
| `GetDefaultGraphics` | `() → s` | |
| `GetSwitchable` | `() → b` | |
| `GetDesktop` | `() → b` | |
| `GetExternalDisplaysRequireDgpu` | `() → b` | |
| `HotPlugDetect` | signal `(t)` | |
| `PowerProfileSwitch` | signal `(s)` | |

Eleven of those concern graphics switching, which this machine cannot do: there
is no discrete GPU. They are constants, not functionality.

The standard interface, `net.hadess.PowerProfiles` and its
`org.freedesktop.UPower.PowerProfiles` alias, is the smaller one: the
`ActiveProfile`, `Profiles`, `Actions`, `ActiveProfileHolds`,
`PerformanceInhibited` and `PerformanceDegraded` properties, the `HoldProfile`
and `ReleaseProfile` methods, and the `ProfileReleased` signal. It accepts
exactly three profile names — `power-saver`, `balanced` and `performance`.

## Diagnostics

```bash
sensors                                              # temperatures and fan speed
journalctl -k -b -1 | grep -i therm                  # throttling in the previous boot
journalctl --list-boots                              # locate truncated sessions
cat /sys/firmware/acpi/platform_profile              # current profile
grep . /sys/class/powercap/intel-rapl:0/constraint_*_power_limit_uw
grep . /sys/devices/platform/asus-nb-wmi/hwmon/hwmon*/fan1_input
busctl --system introspect com.system76.PowerDaemon /com/system76/PowerDaemon
```
