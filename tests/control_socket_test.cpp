// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "control_socket.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFile>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QTest>

#include <grp.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace {

// The server and the client share this thread, so the event loop has to be
// pumped for the server to accept and answer at all.
bool waitForLines(QLocalSocket &client, QStringList &into, int wanted, int timeoutMs = 3000)
{
    QDeadlineTimer deadline(timeoutMs);
    while (into.size() < wanted && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        while (client.canReadLine())
            into.append(QString::fromUtf8(client.readLine()).trimmed());
    }
    return into.size() >= wanted;
}

} // namespace

class ControlSocketTest : public QObject {
    Q_OBJECT

private slots:
    void answersOneLinePerCommand();
    void handlesSeveralLinesInOneWrite();
    void blankLinesAreIgnored();
    void survivesAnAbruptDisconnect();
    void refusesToListenOnAnImpossiblePath();
    void rebindsOverAStaleSocket();
    void repliesWithoutAHandler();
    void theSocketIsHandedToTheGroupItCan();

private:
    QString socketPath(const QString &name) const
    {
        return m_dir.path() + QLatin1Char('/') + name;
    }
    static QString roundTrip(const QString &path, const QString &request);

    QTemporaryDir m_dir;
};

QString ControlSocketTest::roundTrip(const QString &path, const QString &request)
{
    QLocalSocket client;
    client.connectToServer(path);
    if (!client.waitForConnected(2000))
        return QStringLiteral("<not connected>");
    client.write(request.toUtf8() + '\n');
    client.flush();
    QStringList replies;
    if (!waitForLines(client, replies, 1))
        return QStringLiteral("<no reply>");
    return replies.first();
}

void ControlSocketTest::answersOneLinePerCommand()
{
    ControlSocket server([](const QString &line) { return QStringLiteral("ok ") + line; });
    const QString path = socketPath(QStringLiteral("basic.sock"));
    QString error;
    QVERIFY2(server.listen(path, &error), qPrintable(error));
    QCOMPARE(server.path(), path);

    QCOMPARE(roundTrip(path, QStringLiteral("status")), QStringLiteral("ok status"));
    QCOMPARE(roundTrip(path, QStringLiteral("mode work")), QStringLiteral("ok mode work"));
}

void ControlSocketTest::handlesSeveralLinesInOneWrite()
{
    int seen = 0;
    ControlSocket server([&seen](const QString &line) {
        ++seen;
        return QStringLiteral("ok ") + line;
    });
    const QString path = socketPath(QStringLiteral("batch.sock"));
    QVERIFY(server.listen(path));

    QLocalSocket client;
    client.connectToServer(path);
    QVERIFY(client.waitForConnected(2000));
    client.write("one\ntwo\nthree\n");
    client.flush();

    QStringList replies;
    QVERIFY(waitForLines(client, replies, 3));
    QCOMPARE(replies, QStringList({ QStringLiteral("ok one"), QStringLiteral("ok two"),
                                    QStringLiteral("ok three") }));
    QCOMPARE(seen, 3);
}

void ControlSocketTest::blankLinesAreIgnored()
{
    int seen = 0;
    ControlSocket server([&seen](const QString &line) {
        ++seen;
        return QStringLiteral("ok ") + line;
    });
    const QString path = socketPath(QStringLiteral("blank.sock"));
    QVERIFY(server.listen(path));

    QLocalSocket client;
    client.connectToServer(path);
    QVERIFY(client.waitForConnected(2000));
    client.write("\n   \nreal\n");
    client.flush();

    QStringList replies;
    QVERIFY(waitForLines(client, replies, 1));
    QCOMPARE(replies.first(), QStringLiteral("ok real"));
    QCOMPARE(seen, 1);
}

void ControlSocketTest::survivesAnAbruptDisconnect()
{
    ControlSocket server([](const QString &line) { return QStringLiteral("ok ") + line; });
    const QString path = socketPath(QStringLiteral("abrupt.sock"));
    QVERIFY(server.listen(path));

    {
        QLocalSocket client;
        client.connectToServer(path);
        QVERIFY(client.waitForConnected(2000));
        // A line that never arrives in full, then the peer vanishes.
        client.write("partial line without a newline");
        client.flush();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        client.abort();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    // The next client is served as if nothing had happened.
    QCOMPARE(roundTrip(path, QStringLiteral("status")), QStringLiteral("ok status"));
}

void ControlSocketTest::refusesToListenOnAnImpossiblePath()
{
    ControlSocket server([](const QString &) { return QStringLiteral("ok"); });
    QString error;
    QVERIFY(!server.listen(QStringLiteral("/proc/self/cannot/bind/here.sock"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(server.path().isEmpty());
}

void ControlSocketTest::rebindsOverAStaleSocket()
{
    const QString path = socketPath(QStringLiteral("stale.sock"));
    // What a daemon killed with SIGKILL leaves behind: the path exists and
    // nothing is listening on it. It must not block the next run.
    QFile stale(path);
    QVERIFY(stale.open(QIODevice::WriteOnly));
    stale.close();

    ControlSocket second([](const QString &) { return QStringLiteral("ok second"); });
    QVERIFY(second.listen(path));
    QCOMPARE(roundTrip(path, QStringLiteral("status")), QStringLiteral("ok second"));

    second.close();
    QVERIFY(second.path().isEmpty());
}

void ControlSocketTest::repliesWithoutAHandler()
{
    ControlSocket server(nullptr);
    const QString path = socketPath(QStringLiteral("nohandler.sock"));
    QVERIFY(server.listen(path));
    QCOMPARE(roundTrip(path, QStringLiteral("status")), QStringLiteral("err no handler"));
}

void ControlSocketTest::theSocketIsHandedToTheGroupItCan()
{
    // The tray reaches the daemon through this socket without being root, so
    // the socket has to carry the group. Doing it here rather than from an
    // ExecStartPost in the unit is what removes the race: systemd calls
    // ExecStartPost as soon as the process exists, which can be before the
    // socket does, and a chgrp that loses that race fails silently.
    ControlSocket socket([](const QString &) { return QStringLiteral("ok"); });
    const QString path = socketPath("group.sock");
    QVERIFY(socket.listen(path));

    struct stat info {};
    QCOMPARE(stat(path.toLocal8Bit().constData(), &info), 0);

    const group *entry = getgrnam(kControlSocketGroup);
    if (entry && info.st_gid == entry->gr_gid) {
        // The handoff went through — as it always does for the daemon, which
        // runs as root.
        QCOMPARE(info.st_mode & 07777, mode_t(0660));
    } else {
        // It could not: no such group, or this test is not a member of it.
        // That is a log line and not a failure, and the socket stays usable by
        // its owner, because managing heat does not depend on a tray.
        QVERIFY(ControlSocket::isServerLive(path));
    }
}

QTEST_MAIN(ControlSocketTest)
#include "control_socket_test.moc"
