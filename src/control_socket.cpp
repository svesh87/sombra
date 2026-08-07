// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "control_socket.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>

#include <grp.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

// Owner and group may drive the daemon; nobody else may connect. Connecting to
// a unix socket needs write permission on it, so this is the whole access
// control.
constexpr mode_t kSocketMode = 0660;

} // namespace

ControlSocket::ControlSocket(Handler handler, QObject *parent)
    : QObject(parent)
    , m_handler(std::move(handler))
{
    connect(&m_server, &QLocalServer::newConnection, this, &ControlSocket::onNewConnection);
}

ControlSocket::~ControlSocket()
{
    close();
}

bool ControlSocket::isServerLive(const QString &path)
{
    QLocalSocket probe;
    probe.connectToServer(path);
    if (!probe.waitForConnected(200))
        return false;
    probe.abort();
    return true;
}

bool ControlSocket::listen(const QString &path, QString *error)
{
    close();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QLocalServer::removeServer(path);
    if (!m_server.listen(path)) {
        if (error)
            *error = m_server.errorString();
        return false;
    }
    m_path = path;

    const QByteArray native = QFile::encodeName(m_server.fullServerName());
    if (const group *entry = getgrnam(kControlSocketGroup)) {
        // -1 leaves the owner alone; only the group changes.
        if (chown(native.constData(), static_cast<uid_t>(-1), entry->gr_gid) != 0
            || chmod(native.constData(), kSocketMode) != 0) {
            qWarning().noquote() << "control socket could not be handed to group"
                                 << kControlSocketGroup << "— only root can drive the daemon";
        }
    } else {
        qWarning().noquote() << "group" << kControlSocketGroup
                             << "does not exist, so only root can drive the daemon";
    }
    return true;
}

void ControlSocket::close()
{
    if (m_server.isListening())
        m_server.close();
    if (!m_path.isEmpty()) {
        QLocalServer::removeServer(m_path);
        m_path.clear();
    }
}

void ControlSocket::onNewConnection()
{
    while (QLocalSocket *client = m_server.nextPendingConnection()) {
        connect(client, &QLocalSocket::disconnected, client, &QLocalSocket::deleteLater);
        connect(client, &QLocalSocket::readyRead, client, [this, client]() {
            while (client->canReadLine()) {
                const QString line = QString::fromUtf8(client->readLine()).trimmed();
                if (line.isEmpty())
                    continue;
                const QString reply =
                        m_handler ? m_handler(line) : QStringLiteral("err no handler");
                client->write(reply.toUtf8());
                client->write("\n");
                client->flush();
            }
        });
    }
}
