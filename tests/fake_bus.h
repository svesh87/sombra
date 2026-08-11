// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QDBusArgument>
#include <QDBusConnection>
#include <QMap>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <memory>

// A bus of the suite's own, and something on it that answers for polkit.
//
// No test goes anywhere near the system bus. Claiming the daemon's real name there
// would replace the service the machine is running, and the answer to "may this
// caller act" would come from the real policy on somebody's desktop.
//
// The two wire types below are polkit's, declared here rather than shared with the
// daemon on purpose: what the two ends have in common is the signature on the wire,
// not a header. A test that reproduced the daemon's own declaration would pass while
// agreeing with itself.
namespace testbus {

// The subject of the question: `(sa{sv})`.
struct Subject {
    QString kind;
    QVariantMap details;
};

// The answer: `(bba{ss})`.
struct Result {
    bool authorized = false;
    bool challenge = false;
    QMap<QString, QString> details;
};

QDBusArgument &operator<<(QDBusArgument &argument, const Subject &subject);
const QDBusArgument &operator>>(const QDBusArgument &argument, Subject &subject);
QDBusArgument &operator<<(QDBusArgument &argument, const Result &result);
const QDBusArgument &operator>>(const QDBusArgument &argument, Result &result);

} // namespace testbus

Q_DECLARE_METATYPE(testbus::Subject)
Q_DECLARE_METATYPE(testbus::Result)

// What one interface of an introspection document declares. Comparing whole maps is
// what turns "the wire is described in one place" into a check rather than a claim.
// Signatures rather than argument names: a definition names its arguments for whoever
// reads it, and a metaobject does not have to agree about that.
struct Introspection {
    // Method name -> "<input signature> -> <output signature>".
    QMap<QString, QString> methods;
    // Signal name -> concatenated argument signature.
    QMap<QString, QString> signals_;
    // Property name -> "<signature> <access>".
    QMap<QString, QString> properties;
};

Introspection parseIntrospection(const QString &xml, const QString &interfaceName);

// One `dbus-daemon`, started and reaped by the test that owns it.
class PrivateBus {
public:
    ~PrivateBus();

    // Starts it and reads the address it prints. False with a reason when there is no
    // `dbus-daemon` to start or it says nothing.
    bool start(QString *error = nullptr);
    // Starts it on an address decided here, which is the only way to hand that address
    // to somebody before the bus exists. The socket file is removed first:
    // `dbus-daemon` will not bind over one that is already there.
    bool startAt(const QString &socketPath, QString *error = nullptr);
    const QString &address() const { return m_address; }

private:
    QProcess m_process;
    QString m_address;
};

// The daemon as its own process, which is what it is on a machine as well. It cannot
// be an object in a test that also drives a client: a client waiting for a reply and a
// daemon that has to reach its own event loop to answer would be the same thread.
//
// What it waits for is the **name**, not the process. A daemon that started and failed
// to claim anything looks exactly like a slow one otherwise, and the log does not say
// which it was.
class TestDaemon {
public:
    TestDaemon(const QString &sysfsRoot, QString workDir, QString busAddress);
    ~TestDaemon();

    bool waitUntilAnswering(QString *error = nullptr);
    // SIGTERM, which is how systemd stops it: the fan goes back to automatic on the
    // way out.
    void stop();

    static QString binary();

private:
    QString m_work;
    QString m_address;
    QProcess m_process;
};

// polkit, replaced by something a test can steer.
//
// Everything that changes the machine's state is checked against an action first, so
// a suite without an authority would be a suite where every command is refused. This
// claims `org.freedesktop.PolicyKit1` on a private bus and answers yes — or no, which
// is how the refusal path is tested rather than assumed.
class FakeAuthority : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.PolicyKit1.Authority")

public:
    explicit FakeAuthority(QObject *parent = nullptr);
    ~FakeAuthority() override;

    // Connects to `address`, exports itself and claims the name.
    bool start(const QString &address, QString *error = nullptr);

    void setAllowed(bool allowed) { m_allowed = allowed; }
    // Every action asked about, in order, and the unique bus names they were asked
    // for: the subject has to be the caller rather than the daemon.
    QStringList actions() const { return m_actions; }
    QStringList subjects() const { return m_subjects; }
    // Whether anybody ever asked for a dialog. Nothing here ever may: an action
    // answered by `allow_active` has nothing to ask a person.
    bool interactionAsked() const { return m_interactionAsked; }

public slots:
    testbus::Result CheckAuthorization(const testbus::Subject &subject, const QString &action,
                                       const QMap<QString, QString> &details, uint flags,
                                       const QString &cancellationId);

private:
    QString m_connectionName;
    QDBusConnection m_connection { QString() };
    bool m_allowed = true;
    bool m_interactionAsked = false;
    QStringList m_actions;
    QStringList m_subjects;
};
