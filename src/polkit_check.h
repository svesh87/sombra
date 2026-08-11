// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QDBusConnection>
#include <QMap>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <functional>

// "May this caller do this?", asked of polkit over the same bus the caller arrived
// on.
//
// The subject is the caller's **unique** bus name, not the well-known one the daemon
// answers under: the question is about who is calling, and polkit resolves that name
// to a session and a user itself. The interaction flag is zero, always, so no dialog
// can ever appear — the action this daemon guards is answered by
// `allow_active=yes` or not at all.
//
// The answer arrives asynchronously because polkit is a service on the same bus. A
// blocking call inside a method's own handler would be the daemon waiting for polkit
// while polkit waits for the daemon, so the method that asks has to delay its reply.
namespace polkit {

// What polkit's Authority answers with. `challenge` is never true here: with
// interaction disabled and no `auth_*` in the action, polkit either allows or
// refuses.
struct Result {
    bool authorized = false;
    bool challenge = false;
    QMap<QString, QString> details;
};

// The subject of the question: kind plus its details, which for a bus name is a
// single entry, "name".
struct Subject {
    QString kind;
    QVariantMap details;
};

// Registers the wire types both structures need, including `QMap<QString,QString>`,
// which D-Bus does not know by default and which sits inside the result. An
// unregistered type inside a structure fails the whole call with "Marshalling
// failed", and the message does not point at the cause.
void registerTypes();

using Answer = std::function<void(bool allowed, const QString &error)>;

// Asks, and calls `answer` when polkit replies. `context` owns the watcher, so a
// daemon shutting down while a question is outstanding does not answer into a
// destroyed object. An unreachable or silent polkit is a refusal: this runs as root
// and a failure to establish that somebody may act is not permission to act.
void check(const QDBusConnection &connection, const QString &busName, const QString &action,
           QObject *context, Answer answer);

} // namespace polkit
