// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QJsonObject>
#include <QString>
#include <QVariantMap>

// The names the daemon and its two clients meet on, and the translation between
// the bus and the JSON the command line prints.
//
// The daemon assembles its state as one JSON object, which is the description of
// that state and is where absent readings, units and rounding are decided. The bus
// carries it as properties, because a property is what a desktop asks for and what
// announces its own changes. The two halves below are that split and its inverse,
// kept in one file so that a change to either is written next to the other, and
// proved lossless by a round-trip test rather than by inspection.
namespace wire {

// The daemon's own name on the system bus. The three power-profile names in
// `dbus_service.h` are a different surface: they exist so the desktop's panel
// applet can talk to this daemon, and they say nothing about backlight, monitors
// or telemetry.
inline constexpr const char *kService = "io.github.svesh87.Sombra";
inline constexpr const char *kPath = "/io/github/svesh87/Sombra";
inline constexpr const char *kInterface = "io.github.svesh87.Sombra";
// One action for everything that changes state. `allow_active=yes`, so the person
// at this screen is answered without a dialog and a remote or idle session is not.
inline constexpr const char *kAction = "io.github.svesh87.Sombra.manage";

// The property whose value moves every tick. Everything else changes when
// somebody changes it.
inline constexpr const char *kReadings = "Readings";

// Splits the daemon's state into the properties the bus carries.
QVariantMap propertiesFrom(const QJsonObject &status, const QJsonObject &configuration);

// Puts them back together, for a client that wants the whole picture: the console
// builds its window from this, and `sombra status` prints it.
QJsonObject statusJsonFrom(const QVariantMap &properties);

// What a client is asking for. Both clients speak the same vocabulary — the words
// `sombra` takes on its command line and the words the console shows on its status
// line — because a person reading "set pl1=30 → ok" in the console has to be able
// to type the same thing in a terminal. That vocabulary is turned into a typed call
// here, once, so the two clients cannot come to mean different things by it.
struct Call {
    // Reading commands are properties rather than calls: nothing on the bus needs
    // asking for what it is already announcing.
    enum class Read {
        None,
        Status,
        Configuration,
        Monitors,
    };

    Read read = Read::None;
    QString method;
    QVariantList arguments;
    // Why the words make no sense, for a client to print. Set only when neither a
    // read nor a method came out of them.
    QString error;

    bool valid() const { return read != Read::None || !method.isEmpty(); }
};

Call callFor(const QStringList &words);

// And the inverse, on the daemon's side: the command the daemon's own parser takes.
// It stays the entry point for everything, because that parser is where a value is
// validated, clamped and refused, and a second path into the same state would be a
// second set of rules. Empty with a reason set when the arguments cannot be spelled
// as a command.
QString commandFor(const QString &method, const QVariantList &arguments, QString *error);

} // namespace wire
