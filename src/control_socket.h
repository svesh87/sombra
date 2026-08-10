// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QLocalServer>
#include <QObject>
#include <QString>

#include <functional>

// The daemon's control channel: one line in, one line out.
//
// A line protocol rather than D-Bus because this is the channel the tray uses
// for live tuning at 1 Hz and because it has to work before the D-Bus layer
// exists. The D-Bus names are a compatibility surface for the desktop panel,
// not the primary interface.

// The group the socket is handed to. Members of it can drive the daemon, which
// is exactly what the tray needs and nothing more. Shared with the tray so
// that the daemon and the message the tray shows when it cannot connect can
// never name different groups.
inline constexpr const char *kControlSocketGroup = "sombra";
class ControlSocket : public QObject {
    Q_OBJECT

public:
    using Handler = std::function<QString(const QString &line)>;

    explicit ControlSocket(Handler handler, QObject *parent = nullptr);
    ~ControlSocket() override;

    // True when something is already answering on that path. A live socket
    // means another daemon owns the control surface, and two writers is the
    // one thing this architecture forbids, so the caller refuses to start
    // rather than unlinking it.
    static bool isServerLive(const QString &path);

    // Removes a stale socket left by a previous run, binds, and hands the
    // socket to `group` at mode 0660 so the tray can reach it without being
    // root.
    //
    // Done here rather than from the unit file: an ExecStartPost races the
    // daemon's own bind — systemd considers a Type=simple service started as
    // soon as the process exists, which is before this function has run — and
    // a chgrp that loses that race fails silently, leaving a socket nothing
    // but root can connect to.
    //
    // A group that does not exist is a log line, not a failure: the daemon's
    // job is managing heat, and it does that whether or not a tray can reach
    // it. The socket then keeps the mode the umask gave it, which is
    // root-only, and that is the safe direction to fail in.
    bool listen(const QString &path, QString *error = nullptr);
    void close();

    const QString &path() const { return m_path; }

private slots:
    void onNewConnection();

private:
    Handler m_handler;
    QLocalServer m_server;
    QString m_path;
};
