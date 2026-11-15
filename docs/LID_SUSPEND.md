*English · [Русский](LID_SUSPEND.ru.md) · [Español](LID_SUSPEND.es.md)*

# When closing the lid stops suspending the machine

How to tell from the logs who put the machine to sleep, and where to look when
the lid stops doing it. The daemon has no part in this: its code contains no lid
handling, no suspend calls and no inhibitor locks.

## Reading the logind journal

The two reasons to suspend look different in `journalctl -u systemd-logind`, and
this is the only way to tell them apart from the log:

- `Lid closed.` followed by `Suspending...` — the lid;
- `The system will suspend now!` — a `Suspend()` call over the bus (idle timeout,
  the session menu).

A trap: on resume from s2idle the kernel replays the lid state, and the journal
shows a `Lid closed.` / `Lid opened.` pair **within the same second**. That is not
a lid closure. Check it against the kernel's `PM: suspend entry` /
`PM: suspend exit` lines: the replayed pair sits exactly on `exit`.

## Baseline: logind with stock settings

On this machine logind handles the lid with its defaults:

- `logind.conf` is empty and there are no drop-in directories;
- `HandleLidSwitch=suspend`, `HandleLidSwitchDocked=ignore`;
- the lid device is tagged `power-switch`, and logind reports
  `Watching system buttons on /dev/input/event0 (Lid Switch)`.

A healthy closure reads `Lid closed.` -> `Suspending...` ->
`PM: suspend entry (s2idle)`, with `LidClosed` on logind changing in step with
`/proc/acpi/button/lid/LID/state`.

## Symptom and first checks

The marker: closing the lid produces no `Lid closed.` line in the logind journal.
Run, while the lid state is still the one in question:

```
systemd-inhibit --list | grep handle-lid-switch
busctl get-property org.freedesktop.login1 /org/freedesktop/login1 \
  org.freedesktop.login1.Manager HandleLidSwitch LidClosed
cat /proc/acpi/button/lid/LID/state
```

If ACPI says `closed` and logind stays silent, the event does not reach logind,
and the question is who holds the input device. The state of the inhibitor
locks is not written to the journal and does not survive a reboot, so it has to
be captured while the fault is present.

## Where to look

- **The compositor.** `cosmic-comp` can take the lid away from logind: its binary
  contains `Inhibiting lid switch`, `Removing inhibitor-lock on lid switch` and
  `handle-lid-switch`. It also keeps `/dev/input/event0` open and talks to
  `com.system76.PowerDaemon`, a name this daemon answers on. This is a lead, not
  a confirmed cause: an inhibitor lock held by the compositor is what to look
  for in the first check above.
- **Not `system76-power`.** Its package ships a binary, a unit, a D-Bus
  configuration, an interface description and a polkit policy — no udev rules,
  no logind drop-ins, nothing about the lid. Masking its unit has no effect on
  the lid.
- Raw events from `/dev/input/event0` can only be read by `root` or members of
  the `input` group.
