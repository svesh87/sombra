// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "polkit_check.h"

#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

Q_DECLARE_METATYPE(polkit::Result)
Q_DECLARE_METATYPE(polkit::Subject)

namespace polkit {

QDBusArgument &operator<<(QDBusArgument &argument, const Subject &subject)
{
    argument.beginStructure();
    argument << subject.kind << subject.details;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, Subject &subject)
{
    argument.beginStructure();
    argument >> subject.kind >> subject.details;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const Result &result)
{
    argument.beginStructure();
    argument << result.authorized << result.challenge << result.details;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, Result &result)
{
    argument.beginStructure();
    argument >> result.authorized >> result.challenge >> result.details;
    argument.endStructure();
    return argument;
}

} // namespace polkit

namespace {

constexpr const char *kService = "org.freedesktop.PolicyKit1";
constexpr const char *kPath = "/org/freedesktop/PolicyKit1/Authority";
constexpr const char *kInterface = "org.freedesktop.PolicyKit1.Authority";
// The caller's unique name is the subject. A well-known name would be the wrong
// answer twice over: it can move to another process, and the daemon's own name is
// not who is asking.
constexpr const char *kSubjectKind = "system-bus-name";
// No dialog, ever. `AllowUserInteraction` is bit zero.
constexpr uint kNoInteraction = 0;
constexpr int kTimeoutMs = 5000;

} // namespace

namespace polkit {

void registerTypes()
{
    qDBusRegisterMetaType<Subject>();
    qDBusRegisterMetaType<Result>();
    // Inside Result, and unknown to D-Bus until somebody says so.
    qDBusRegisterMetaType<QMap<QString, QString>>();
}

void check(const QDBusConnection &connection, const QString &busName, const QString &action,
           QObject *context, Answer answer)
{
    Subject subject;
    subject.kind = QString::fromLatin1(kSubjectKind);
    subject.details.insert(QStringLiteral("name"), busName);

    QDBusMessage message = QDBusMessage::createMethodCall(
            QString::fromLatin1(kService), QString::fromLatin1(kPath),
            QString::fromLatin1(kInterface), QStringLiteral("CheckAuthorization"));
    message.setArguments({ QVariant::fromValue(subject), action,
                           QVariant::fromValue(QMap<QString, QString>()), kNoInteraction,
                           QString() });

    auto *watcher = new QDBusPendingCallWatcher(connection.asyncCall(message, kTimeoutMs), context);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context,
                     [answer = std::move(answer)](QDBusPendingCallWatcher *watcher) {
                         const QDBusPendingReply<Result> reply = *watcher;
                         watcher->deleteLater();
                         if (reply.isError()) {
                             answer(false, reply.error().message());
                             return;
                         }
                         answer(reply.value().authorized, QString());
                     });
}

} // namespace polkit
