// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

// The other end of `io.github.svesh87.Sombra`, shared by the console and the command
// line.
//
// Reading is not polling: everything the daemon publishes is read once with `GetAll`
// and kept, and after that the daemon says what changed. The console therefore shows
// a value that moved as soon as it moves, and asks for nothing once a second.
//
// A daemon that is not running is an ordinary state rather than an error. The name
// having an owner is watched, so a daemon that is restarted under an open console is
// noticed both ways round without a timer looking for it.
class BusClient : public QObject {
    Q_OBJECT

public:
    // Empty means the system bus. An address points this at a bus of its own, which
    // is how the suite runs without touching the one the desktop is on.
    explicit BusClient(QString busAddress = QString(), QObject *parent = nullptr);
    ~BusClient() override;

    // Whether the daemon holds its name right now.
    bool available() const;

    // Everything the daemon publishes, read in one exchange. Empty when it is not
    // answering, which is the same answer as "there is no daemon".
    QVariantMap readAll();

    // One method, and the line it answers with: "ok" or "ok <what happened>", and
    // "err <why>" for a refusal. The same sentence the command line prints and the
    // console puts on its status line, so the two cannot disagree about what the
    // daemon said.
    QString call(const QString &method, const QVariantList &arguments);

signals:
    // The keys that changed, as the daemon announced them. A client merges them into
    // what it already has: what is not named has not changed.
    void propertiesChanged(const QVariantMap &changed);
    void availabilityChanged(bool available);

private slots:
    // Connected by name rather than through a generated proxy, so that nothing waits
    // on introspection succeeding first.
    void onPropertiesChanged(const QString &interfaceName, const QVariantMap &changed,
                             const QStringList &invalidated);

private:
    class Private;
    std::unique_ptr<Private> d;
};
