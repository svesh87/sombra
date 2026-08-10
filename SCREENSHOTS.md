*English · [Русский](SCREENSHOTS.ru.md)*

# The interface

All of it belongs to `ombra-tray`. The daemon itself has no interface: it answers
on a socket and on three D-Bus names.

## The tray menu

![The tray menu](screenshots/tray-menu.png)

The first line is the current mode with the package temperature and the fan
speed beside it, because that is what a glance at the panel is for. Below it the
modes as the daemon has them saved — the menu is built from a status reply, not
compiled in — then the console and the way out.

The icon says which mode is in force, by the names desktop power-profile
applets use (`power-profile-power-saver-symbolic` for the coolest envelope,
`-balanced-` for the working one, `-performance-` for the widest), plus two
names that are not modes: `action-unavailable-symbolic` when the daemon is not
answering and `dialog-warning-symbolic` when the emergency guard is clamping. A
name rather than a picture, because the panel then draws the glyph of the icon
theme in force, recolours it to match itself, and follows a change of icon
theme.

The catch, and the reason there is code behind this at all: a
StatusNotifierItem host prefers the name over any picture sent with it, so a
name its theme does not carry becomes a question mark in the panel. So the name
is published only when the theme has it, and otherwise the icon is a picture of
ours — a chip whose fill says which mode is running, drawn in fixed colours that
read on a light panel and on a dark one. The palette is not consulted for it:
the palette belongs to the application, the panel is coloured by the desktop,
and the two disagree exactly when the desktop is light with a dark bar.

For that check to mean anything, Qt has to look names up in the theme the panel
uses. It does not by default — the platform theme in use names an icon theme of
its own — so the console reads the desktop's icon theme from the portal
(`org.gnome.desktop.interface` / `icon-theme`) and points Qt at it. A change of
icon theme arrives as a portal signal, and the icon is chosen again.

The console's own light or dark is a choice on the *Settings* tab: follow the
desktop, or light, or dark. Following the desktop reads the freedesktop
appearance portal rather than Qt — `QStyleHints::colorScheme()` needs Qt 6.5 and
the Qt here is 6.4, and the platform theme takes its colours from its own
configuration and never looks at the desktop. A palette of ours is applied only
when the current one is of the other scheme, so on a correctly configured
desktop following the desktop changes nothing at all. None of this reaches the
panel icon: what the panel draws is the desktop's business.

## The console

Four tabs: what only reports, the thermal mode, the backlight, and the console
itself. The split is by subject. Splitting by how long a change lasts instead —
the mode, the saved definitions, the live sliders — puts the same eight numbers
in three places, because "applies now" and "written down" are two properties of
one thing, not two things.

The connection line sits outside the tabs: whether the daemon is answering is
not something to go looking for on another tab. The daemon's reply to the last
command appears under them, on the same terms — and only once there is one,
because a reply line kept visible from the start is a strip of empty space along
the bottom of every tab.

### Now — what only reports

![The Now tab](screenshots/console-now.png)

The readings, the indicators for the states worth knowing about, and the three
verdict buttons. The line under *State* here is an indicator doing its job: the
controls have been moved, so what is running is not one of the saved modes.

Good, too loud and too slow stamp a judgement onto the current sample. They are
on this tab because a judgement is about what the machine is doing right now,
and the readings that produced it are on screen beside them.

### Mode — one tab, in the order it is used

![The Mode tab](screenshots/console-mode.png)

Pick a mode and the controls take its numbers. Move one and it applies at once,
and the mode selector empties — what is running is no longer any of the three,
and pretending otherwise would be a lie about the machine. *Save to* then writes
the controls into a mode you name, which is the only way any of this becomes
durable.

The selector follows the desktop panel as well: switching there moves the
controls here, so the panel and the console never disagree about what is
running.

Every number has a field beside its slider. A slider cannot be landed on a
particular value, and these values are compared between sessions and pasted into
notes.

The fan is one control with four answers: the three curves the firmware carries,
and full speed. Full speed is roughly 6100 RPM, and having it in the same list
is what makes "it is too hot right now" one click rather than a command line.

### Backlight — a separate function

![The Backlight tab](screenshots/console-backlight.png)

Its own tab, deliberately not beside the mode: selecting a thermal mode never
moves the backlight, and nothing about the backlight reaches the thermal side.

Three sliders, all in per cent. *Brightness* is the ordinary control and the
level it leaves behind is the baseline everything else works around; *Reaction
to light* is how far the room may move it either way; *Never darker than* is the
floor, which binds the automation and a slip of the hand alike. There is no
ceiling, because that is 100 %, and there are no profiles: one room, one set of
settings.

Nothing about the sensor is on screen. What the room measures in lux is the
daemon's business, and the curve that reads it is calibrated from recorded
telemetry rather than judged on the spot.

Everything that sends a command greys out while nothing is answering, on every
tab: a slider parked at its minimum is otherwise indistinguishable from a daemon
holding 1 W.

Each picture is fitted to its own tab. In use the window takes the size of its
largest tab and keeps it: every tab fits, so switching moves nothing, and the
frame does not resize — what dragging it could produce is either empty space or
a scrollbar over content that had room. A photograph of one size for all four
would carry a field of nothing under the short tabs, which is why the pictures
are the one place that size is undone.

### Settings — about the program rather than the machine

![The Settings tab](screenshots/console-settings.png)

What this window looks like, whether it starts with the session, and the one
button that discards every setting there is.

*Appearance* is light, dark, or whatever the desktop says, kept in the console's
own settings file. *Start with the session* writes an XDG autostart entry and
nothing else — and the entry *is* the state, which is why something switched off
stays off with nothing to bring it back. It takes effect at the next login,
because the session reads autostart entries when it starts, and the window says
so rather than looking as though the click did nothing.

*Reset everything* is the way out of a mess: every mode definition and every
backlight setting back to the values compiled into the daemon. It asks first. It
is here rather than beside the numbers it discards because it covers all of
them, it is used about once, and on the mode tab it cost height the controls
above it needed.

## Making them again

```bash
make
./scripts/screenshots.sh
```

One command, all five files — the menu and one per tab — run from the
repository root.

The daemon it photographs is a throwaway one, started by that script against a
fake control surface: a directory of ordinary files built from the same header
the test suite uses. It therefore owns nothing and writes to nothing, and three
things follow. The readings are identical every time, so regenerating the
pictures is not a diff of somebody's afternoon temperatures. Nothing measured
on a real machine ends up in a public repository. And every command the console
asks about is answered, including the saved definitions, which an older
installed build would not know.

Nothing but the widget goes into the file either: these are grabs of our own
windows rather than of the screen, so no compositor and no screenshot tool is
involved and nothing else that happened to be on screen can end up here.

Before committing new pictures, look at them. The console names the socket path,
and from this script that is always `/tmp/ombra-screenshots/control.sock` — the
throwaway one. No host name, no user name and no path from a developer's home
belongs in this repository.

**A change to the interface is not finished until these are regenerated.** A
screenshot goes stale silently — no test catches it, and the reader is left
looking at controls that are no longer there.
