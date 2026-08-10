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

`docs/HARDWARE.md` is the source of truth about the machine. Read it before touching
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
- **A translation is written as a text in its own language, not word by word.**
  The original's syntax does not carry across: a construction like "which is
  what this exists to" becomes a fragment in Russian that attaches to nothing.
  The test is to read the sentence aloud away from the original; if it does not
  read, rewrite it whole instead of patching a word.
- **The finished translation is proofread as a document of its own**, without
  the original in view, and only then checked against the original for meaning.
  False friends and dropped words — `underused` rendered as "underrated", a
  missing `else` — stay plausible and survive a word-by-word comparison.
- **One term, one translation across the repository.** The same word rendered
  differently in neighbouring documents reads as two different things. Do not
  invent words the language does not have: a calque looks like a typo rather
  than a term, and where no short equivalent exists, describe it instead.
- The final consistency pass checks the pairs against each other.

## The goal, which is frozen

Change only on an explicit request from the maintainer. Everything else about
how the project is built is a decision rather than a rule, and those live in
`docs/DESIGN.md` with the reasoning that produced them: they change by an ordinary
edit, in the same change as the code and the documents.

- **What is convenient for the person using it wins.** This is the goal every
  other line here serves, and the one to reach for when two of them appear to
  disagree. A rule that makes the machine correct and the person's day worse has
  been read wrongly.
- **A cool chassis outweighs performance.** This machine has far more headroom
  than the workload requires. When temperature and speed conflict, throttle
  harder.
- **Noise is a constraint, not the goal.** The calibrated scale, measured by
  ear on this machine: up to 3000 RPM is effectively inaudible and is the
  working target at idle; 4000 RPM is audible; 4000–4500 RPM is noticeably
  loud but tolerable, and is the working ceiling under load; 5000–6000 RPM is
  excessive. Nothing reaches full speed without a person having asked for it —
  by the command, by the console, or by putting it in a mode they then select.
  What is forbidden is the machine arriving there on its own, with the
  emergency guard as the single exception.
- **The fan curve is the noise-versus-temperature dial, and it is chosen per
  mode rather than globally.** At identical power settings under sustained load
  the curves separate cleanly: `performance` 74 °C at 5000 RPM, `balanced` 80 °C
  at 3700 RPM, `quiet` above 84 °C at roughly 2900 RPM. At idle the choice makes
  no difference at all, because burst suppression keeps the package below the EC
  ramp threshold and every curve leaves the fan at its 1900 RPM floor. Select
  the curve from each mode's noise budget; there is no globally correct answer
  and no curve is forbidden.

Read `docs/DESIGN.md` before proposing anything structural. A proposal that argues
with a decision recorded there is welcome; one that has not noticed it is not.

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
- **A change to the interface is not finished until the screenshots are
  regenerated.** Same reason as the comments: a picture of controls that are no
  longer there goes stale silently and no test catches it. Regenerating is one
  command, `./scripts/screenshots.sh`; what each picture shows and
  what to check before committing it live in `docs/SCREENSHOTS.md`. Give it its own
  plan step, naming the pictures, not a line saying "update screenshots".
- The **last** step of any plan is a consistency pass across code, comments and
  documentation, fixing what it finds in place. Minimum scope: grep for
  terminology from superseded models; grep for edit-history residue ("used to",
  "now", "no longer"); grep for references to `tmp/` from tracked files; check
  every claim in the root documents against the code that implements it (sysfs
  paths, thresholds, units, partial-failure behaviour, restart behaviour);
  every `*.ru.md` copy against its English original; terminology consistency;
  screenshots regenerated if the diff touched `src/tray_window.cpp` or
  `src/tray_main.cpp`; `git diff --check`.
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
  promoted** into `AGENTS.md`, `README.md` or `docs/HARDWARE.md`. Context files are
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
  `tmp/ARCHITECTURE.md` is updated; copy the previous version to
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
