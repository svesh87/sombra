*English · [Русский](AGENTS.ru.md)*

# Agent rules

`CLAUDE.md` is a symlink to this file — edit the real file only. This file holds
rules that are stable over time. Work status, plans and findings belong in
`tmp/`, never here.

## The project

A daemon that manages the thermal envelope of an ASUS Zenbook 14 UX3405MA
(Core Ultra 9 185H) according to a selected mode, and a tray UI to switch modes.
The daemon also samples sensors once per second and retains the readings in a
bounded ring.

`HARDWARE.md` is the source of truth about the machine. Read it before touching
anything that writes to sysfs.

## This repository is public

- It contains conclusions and decisions, never the research process that
  produced them. Investigation notes, drafts and audits live in `tmp/`, which
  is not tracked.
- No personal data: no names beyond the copyright holder, no email addresses,
  no host names, no absolute paths from a developer's machine, no incident
  narratives.
- Hardware model numbers are scope information, not private data — state them
  plainly so readers can tell whether the findings apply to them.
- Documentation, code comments and identifiers are in English.

### Translations

Each root document has a Russian copy beside it, named `<NAME>.ru.md`.

- **English is canonical.** Where the two disagree, the English file is correct
  and the Russian one is a stale translation to be fixed.
- **A change to an English document is not finished until its Russian copy is
  updated in the same change.** Mirrored documents drift apart otherwise, and a
  translation that contradicts the original is worse than no translation.
- `AGENTS.ru.md` is a convenience copy for human review and is never a source of
  rules: agents follow `AGENTS.md`.
- The final consistency pass checks the pairs against each other.

## Frozen decisions

Change only on an explicit request from the maintainer.

### Goal

- **A cool chassis outweighs performance.** This machine has far more headroom
  than the workload requires. When temperature and speed conflict, throttle
  harder.
- **Noise is a constraint, not the goal.** The calibrated scale, measured by
  ear on this machine: up to 3000 RPM is effectively inaudible and is the
  working target at idle; 4000 RPM is audible; 4000–4500 RPM is noticeably
  loud but tolerable, and is the ceiling for automatic control under load;
  5000–6000 RPM is excessive and is reachable only on explicit demand.
  Automatic control never exceeds 4500 RPM.
- **The fan curve is the noise-versus-temperature dial, and it is chosen per
  mode rather than globally.** At identical power settings under sustained load
  the curves separate cleanly: `performance` 74 °C at 5000 RPM, `balanced` 80 °C
  at 3700 RPM, `quiet` above 84 °C at roughly 2900 RPM. At idle the choice makes
  no difference at all, because burst suppression keeps the package below the EC
  ramp threshold and every curve leaves the fan at its 1900 RPM floor. Select
  the curve from each mode's noise budget; there is no globally correct answer
  and no curve is forbidden.

### Architecture

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
  with its own profile and its own selector.** It reacts to the room, not to
  the power profile, which is why it does not contradict the rule above.
  Nothing about the thermal mode may reach it, in either direction.
- **The level the person set is the baseline, and the automation works in a
  window around it.** A hand correction is an instruction, not an
  interruption: moving the brightness states what level is wanted in this
  light, and the window moves with it. The brightness profile says how far the
  automation may wander from that baseline, and a floor and a ceiling bound it
  absolutely. Those bounds constrain the automation, never the person.
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
- Maximum fan speed (`pwm1_enable=0`) is reachable only by explicit command and
  is never engaged by automatic control.
- Responsibilities inherited along with ownership, and therefore mandatory:
  reacting to AC/battery transitions, and the charge threshold at
  `/sys/class/power_supply/BAT0/charge_control_end_threshold`.

### No automatic regulation

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

### Mode configuration

- **The daemon's text configuration file is the single source of truth.** The
  reason is not convenience: mode parameters are tuned by automated analysis of
  recorded telemetry, and tooling can edit a text file but cannot operate a
  dialog.
- **The daemon reads the configuration and never writes it**, so that comments
  survive. The selected mode lives in a separate state file. Reloading is driven
  by inotify, without a restart.
- **The tray is an experimentation console, not a settings editor.** Two
  different things must not be confused:
  - *durable mode definitions* live in the configuration file and survive a
    reboot;
  - *live tuning* happens in the tray — sliders for the power limit and the CPU
    parameters that apply immediately and are never written back anywhere.

  Live tuning exists because choosing a setpoint is a subjective judgement made
  by ear: the useful question is "does this sound acceptable right now", which
  cannot be answered by editing a file and restarting a daemon. A conventional
  settings dialog that merely edits the config is still forbidden — that would
  be a second way to do what the config already does.
- **The tray never writes the configuration.** When a live setting is worth
  keeping, the tray offers it as a configuration snippet to copy; promoting it
  is a deliberate edit by the maintainer.
- **Every live setpoint is recorded in every telemetry sample** — active mode,
  power limits, frequency ceiling, turbo, EPP, HWP dynamic boost, platform
  profile. A log that does not say what was applied cannot be analysed. Mode and
  setpoint transitions are derived by diffing consecutive samples; do not add a
  separate event log for them.
- **Subjective verdicts are data too.** The tray can stamp the current sample
  with a user mark ("good", "too loud", "too slow"), so that a judgement made by
  ear is later recoverable together with the settings that produced it. Without
  this, live tuning produces an opinion that cannot be traced back to numbers.

### Metrics storage

- **Held in memory, flushed to disk infrequently.** Conserving NVMe endurance is
  deliberate; proposals to write every sample straight to disk are rejected.
- Retention limits are hard: the store does not grow, and eviction is a property
  of the format rather than of a background cleanup task.
- The format is chosen so that mode parameters can be tuned from it and charts
  drawn from it. Averaging that hides peaks is unacceptable.

## Hardware safety (non-negotiable)

- The daemon restores automatic fan control (`pwm1_enable=2`) on every exit
  path: normal shutdown, signal, crash, service restart. A fan left behind by a
  dead daemon is a defect. Because this daemon is the sole owner, nothing else
  will pick up after it.
- The firmware's 200 W PL1 is treated as a fault condition. The daemon never
  raises a limit above a value explicitly confirmed by the maintainer, including
  when returning to a "normal" state.
- Hardware protection thresholds (`coretemp` trip points, `thermald`, ACPI) are
  never disabled or moved.
- Every sysfs node is read and checked for existence before being written:
  `hwmon*` indices move between boots and nodes differ between kernels. A
  missing node is graceful degradation with a log line, not a crash.
- Writing `platform_profile` makes the firmware reassert its own power limits.
  Write the profile first, the limits second, and verify by reading back.
- When sensors are unavailable, the controller falls back to the most
  conservative mode, not to the last known one.

## Quality gates (required before handing over any work)

- lint and static checks clean;
- all tests green;
- line coverage of production code at 80% or above — including the controller,
  sysfs parsing, the metrics store, UI behaviour and error paths;
- the build passes;
- `git diff --check` clean.

Run the full suite after every coherent iteration, not only at the end. Work is
incomplete if a test fails, is unexpectedly skipped, or is flaky, or if coverage
is below the threshold. Weakening assertions, excluding files from coverage, and
marking tests as expected failures to satisfy the gates are all prohibited.

## Test design

- All hardware and sysfs access goes through an explicit interface with a
  substitutable implementation. Tests point the daemon at a fake sysfs tree in a
  temporary directory and exercise it without hardware.
- The controller is tested as a pure function from (telemetry, mode, state) to
  setpoints: escalation, release, hysteresis, sticking, missing sensors,
  malformed values, counter wraparound.
- The metrics store is tested for eviction by depth and by size, for surviving a
  restart, and for tolerating a truncated tail.
- UI tests run on the offscreen platform, without a display.
- Hardware checks supplement automated tests and never replace them.
- Every fixed defect gets a regression test.

## Working with the maintainer

- Non-trivial work starts with a draft plan marked "draft, awaiting approval"
  (file naming and archival under the `tmp/` conventions below); execute only on
  an explicit instruction.
- Genuine forks in a plan — architecture, scope, external configuration — are
  raised in conversation *before* the plan is finalised, and the answers are
  written into the plan as settled decisions. Do not add a "questions" section
  to a plan.
- Minor default choices that are not forks are recorded in the plan marked
  "change at review if you disagree".
- Order of work: code -> documentation and code comments -> gates -> status
  edits (statuses and measured figures only *after* the gates confirm them) ->
  a final consistency pass -> refresh `tmp/CONTEXT.md`.
- Code comments are a work item like any document: give them their own plan step
  with a specific list of file plus anchor, not a line saying "update comments".
  Doc blocks, comments on constants, on controller thresholds and on sysfs paths
  go stale first and go stale silently.
- The **last** step of any plan is a consistency pass across code, comments and
  documentation, fixing what it finds in place. Minimum scope: grep for
  terminology from superseded models; grep for edit-history residue ("used to",
  "now", "no longer"); grep for references to `tmp/` from tracked files; check
  every claim in the root documents against the code that implements it (sysfs
  paths, thresholds, units, partial-failure behaviour, restart behaviour);
  every `*.ru.md` copy against its English original; terminology consistency;
  `git diff --check`.
- If the consistency pass touched code, rerun the gates in full.

## Delegating to subagents

Standing permission from the maintainer: use subagents at your own discretion,
without asking each time.

Subagents increase total token spend rather than reducing it — each one is
briefed from scratch, reports back, and its report is then read and checked.
What they buy is wall-clock time and headroom in the main context. The main
loop's context is cache-warm, so re-reading it costs a fraction of fresh input;
a subagent starts cold and pays full price for the same briefing. **Delegation
therefore pays only when the work is large relative to its briefing** — a
cheaper model on a short task can cost more than doing it in the main loop.

- **Delegate** large, genuinely independent tracks that can be briefed from a
  written contract: the tray once the socket protocol and ring format are
  frozen, the D-Bus compatibility layer, document translations.
- **Delegate for independence, not for capability.** A reviewer with a clean
  context catches what the author is blind to. Give it only the specification
  and the code and ask it to break them. Worth doing for the metrics ring
  format, and for the actuator plus the emergency guard — the two places where
  the cost of an error is real hardware.
- **Do not delegate** interdependent modules whose interfaces are still being
  designed (they will diverge), the controller and safety logic, the quality
  gates, or the final consistency pass — that pass exists to catch divergence
  between pieces and must see all of them at once.
- **Do not delegate work that a handful of tool calls would finish.** The
  briefing costs more than the work.
- Keep the count low. Prefer one subagent to several; brief it precisely the
  first time rather than launching, waiting and re-briefing. Commit to the
  delegation — do not redo its work or re-derive its findings afterwards.
- Escalating to a more capable model is a deferred decision, not a planned one.
  If something proves intractable, escalate then, on that specific question.
  Note that a heavily prescriptive rules file like this one degrades the output
  of the most capable models, which want goals and constraints rather than a
  procedure — escalation would mean re-shaping the briefing, not just switching
  the model.

## Persisted working context

Everything that is not yet a settled decision — investigation, measurements,
reasoning, open questions — lives in files under `tmp/`, never only in a
conversation. A session that ends with its findings only in chat has lost them.

- `tmp/CONTEXT.md` is a **snapshot of the current state**, rewritten at the end
  of every iteration and before handing work back: what the project is doing
  now, which decisions were taken and why, what is still open, which
  measurements exist and where their raw data is, and the next concrete step.
  It is a snapshot, not a log — the previous version goes to `tmp/old/`.
- Anything in `tmp/CONTEXT.md` that has become a **stable decision must be
  promoted** into `AGENTS.md`, `README.md` or `HARDWARE.md`. Context files are
  a staging area; if durable facts accumulate there instead, the public
  documents stay empty and the project's real state becomes untransferable.
- **`tmp/` is not tracked by git**, so a clone of this repository does not carry
  it. Moving the work to another machine, or restoring it from a backup, means
  copying `tmp/` alongside the repository as a separate step. Do not assume the
  public remote preserves working context.

## Audits, fix plans and architecture notes (`tmp/` conventions)

- An audit goes to `tmp/AUDIT_<AI_MODEL>_<date_time>.md`. Before writing a new
  one, move previous audits *from the same model* to `tmp/old/`; leave other
  models' audits alone.
- A summary audit is assembled into `tmp/AUDIT_SUM_<date_time>.md` from recent
  audits by different models, less than an hour old. Previous `AUDIT_SUM_*`
  files move to `tmp/old/`.
- Every audit emphasises simplicity, resistance to speculative extensibility,
  and the absence of surplus robustness. A finding of the form "add protection
  or extensibility for the future" is not a finding — it is a violation of the
  model.
- Work plans go to `tmp/FIX_PLAN_<date_time>.md`; superseded plans move to
  `tmp/old/`, and the current cycle's plan stays in `tmp/` until the next one
  appears.
- External remarks are never transferred into a plan unexamined, whatever their
  source. Verify each finding against the code for accuracy and against the
  simplicity criteria first. Findings that would require additional machinery,
  or that "align" things without a real discrepancy, become documentation-only
  items, reasoned rejections, or questions — not code.
- On request, the full architecture description in
  `tmp/THROTTLER_ARCHITECTURE.md` is updated; copy the previous version to
  `tmp/old/` first.

## Repository rules

- Do not commit or push without an explicit instruction.
- Drafts, plans and temporary investigation belong in `tmp/` only. `tmp/old/` is
  an archive, not a source of requirements.
- Tracked files — code, tests, root documents — never reference `tmp/`: no
  paths, no mentions of files that live there.
- Clean up after yourself in the system `/tmp`: at the end of a session, remove
  files and directories created there during that session. Do not touch anything
  else in `/tmp`; other processes and people keep things there. This does not
  apply to the repository's own `tmp/`.
- Never run local `sudo`, install packages, or perform OS-level setup
  automatically. If a dependency is missing, stop and ask.
- Measurements on live hardware are performed by the maintainer. Never claim
  that a mode, a temperature or a noise level was verified unless the
  verification actually happened.

## Clarify doubtful necessity instead of coding around it

Before building machinery "just in case" — compatibility with old storage
formats, migrations, defences against hypothetical states, support for other
laptop models — ask whether the state being defended against exists. One
question is cheaper than unnecessary code plus tests plus documentation plus
its eventual removal.

## Style

- Comments explain constraints and semantics, not the history of edits.
- Do not tie comments to numeric literals derived from constants; refer to the
  constant names. State units explicitly: sysfs deals in microwatts,
  microseconds and millidegrees, and they are easy to confuse.
