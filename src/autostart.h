// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QString>

// Whether the tray starts with the session.
//
// The state is the file and nothing else: an XDG autostart entry either exists
// or it does not. Nothing here is remembered in a settings file of its own,
// which is what makes "switched off and it stayed off after a reboot" a
// property of the arrangement rather than of some mechanism.
//
// This is the one thing the tray writes. It is not the daemon's configuration —
// no mode definition and no brightness profile is reachable from here — it is
// what the console knows about itself, and the configuration file has no way to
// describe it.
//
// The entry takes effect at the next login: on this desktop the units are made
// by systemd-xdg-autostart-generator, which runs when the session starts. The
// window says so, because a switch that appears to do nothing is worse than no
// switch.
namespace autostart {

// `$XDG_CONFIG_HOME/autostart/sombra-tray.desktop`, honouring the environment so
// that a test can point it somewhere harmless.
QString entryPath();

bool enabled();

// Writes or removes the entry. `Exec` names the running binary, so the entry
// starts the same tray the switch was thrown in. Returns false with a reason:
// a switch that silently failed to take is the defect this guards against.
bool setEnabled(bool on, QString *error = nullptr);

} // namespace autostart
