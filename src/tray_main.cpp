// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "theme.h"
#include "tray_client.h"
#include "tray_icon.h"
#include "tray_window.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QPixmap>
#include <QScrollArea>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTimer>

namespace {

// Where the one-instance lock lives: the session's runtime directory, so it goes
// away with the session and belongs to one person on a shared machine.
QString guardPath()
{
    QString directory = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (directory.isEmpty())
        directory = QDir::tempPath();
    return directory + QStringLiteral("/ombra-tray.sock");
}

// How long an already running console has to answer before this one decides it
// is talking to a socket nobody is behind. Local, and it either answers at once
// or the process that made it is gone.
constexpr int kGuardTimeoutMs = 300;

// How long to wait for a tray host at startup, and how often to ask. The host
// registers over the session bus and is routinely a second or two behind the
// processes an autostart entry launches; a minute covers a slow login without
// leaving a process waiting for a panel that is never coming.
constexpr int kTrayWaitMs = 60000;
constexpr int kTrayPollMs = 500;

QString summaryText(const TrayClient::Status &status)
{
    const QString mode = status.modeLabels.value(status.modeIds.indexOf(status.modeId),
                                                 status.modeId);
    if (status.packageC <= TrayClient::kNoReading)
        return mode;
    return QStringLiteral("%1 — %2 °C, %3 RPM")
            .arg(mode, QString::number(status.packageC, 'f', 1),
                 status.fanRpm <= TrayClient::kNoReading
                         ? QStringLiteral("—")
                         : QString::number(status.fanRpm, 'f', 0));
}

// The mode list belongs to the daemon's configuration file, so the menu is
// built from a status reply rather than compiled in here.
void rebuildModeActions(QMenu &menu, QAction *before, QActionGroup &group, TrayClient &client,
                        const TrayClient::Status &status)
{
    qDeleteAll(group.actions());
    for (int i = 0; i < status.modeIds.size(); ++i) {
        const QString id = status.modeIds.at(i);
        QAction *action = new QAction(status.modeLabels.value(i, id), &group);
        action->setCheckable(true);
        action->setData(id);
        QObject::connect(action, &QAction::triggered, &client,
                         [&client, id]() { client.sendAndRefresh(QStringLiteral("mode ") + id); });
        menu.insertAction(before, action);
    }
}

// Saves the two pictures the documentation carries and says whether it worked.
//
// The grabs are of our own widgets, so no compositor and no screenshot tool are
// involved and nothing but the widget ends up in the file — which also means
// nothing else that happened to be on screen can leak into a public repository.
// A menu is a widget too, which is why the same call takes both.
bool saveScreenshots(const QString &directory, TrayWindow &window, QMenu &menu, QString *error)
{
    const QDir target(directory);
    if (!target.exists()) {
        *error = QStringLiteral("%1 does not exist").arg(directory);
        return false;
    }

    // Shown, because a widget that has never been laid out grabs at the wrong
    // size. WA_DontShowOnScreen lays it out without putting it in front of
    // whoever is running the command.
    menu.setAttribute(Qt::WA_DontShowOnScreen);
    menu.show();
    window.show();
    QApplication::processEvents();

    const auto write = [&](const QString &name, const QPixmap &picture) {
        const QString path = target.filePath(name);
        if (!picture.save(path)) {
            *error = QStringLiteral("cannot write %1").arg(path);
            return false;
        }
        qInfo().noquote() << "wrote" << path;
        return true;
    };

    if (!write(QStringLiteral("tray-menu.png"), menu.grab()))
        return false;

    auto *tabs = window.findChild<QTabWidget *>();
    if (!tabs) {
        *error = QStringLiteral("the console has no tabs to photograph");
        return false;
    }
    for (int i = 0; i < tabs->count(); ++i) {
        tabs->setCurrentIndex(i);
        QApplication::processEvents();
        // Fitted to this tab exactly, which is the window's own business — it
        // is the one that knows why it normally keeps a single fixed size.
        window.fitToCurrentTab();
        QApplication::processEvents();
        if (!write(QStringLiteral("console-%1.png").arg(tabs->tabText(i).toLower()),
                   window.grab())) {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ombra-tray"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    // Governed by waitForTray() below: while there is no icon in a panel, a
    // closed console has to end the process, because nothing else could bring
    // it back.
    QApplication::setQuitOnLastWindowClosed(true);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
            "Experimentation console for ombrad. Sliders apply immediately and are never "
            "written back; durable mode definitions live in the daemon's configuration file."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption socketOption(
            QStringLiteral("socket"), QStringLiteral("Control socket."), QStringLiteral("path"),
            QStringLiteral("/run/ombra/control.sock"));
    const QCommandLineOption intervalOption(
            QStringLiteral("interval-ms"),
            QStringLiteral("How often to ask the daemon for readings."), QStringLiteral("ms"),
            QStringLiteral("1000"));
    // A test hook: it is what lets the suite run this binary against a test
    // daemon and still have it exit.
    const QCommandLineOption quitAfterOption(
            QStringLiteral("quit-after-ms"),
            QStringLiteral("Exit after this many milliseconds instead of running until quit."),
            QStringLiteral("ms"), QStringLiteral("0"));
    // The documentation's pictures are generated rather than taken by hand: a
    // screenshot of an interface goes stale silently, and regenerating it has
    // to be one command or it will not happen.
    const QCommandLineOption screenshotOption(
            QStringLiteral("screenshot"),
            QStringLiteral("Write tray-menu.png and one console-<tab>.png per tab to this "
                           "directory and exit."),
            QStringLiteral("directory"));
    parser.addOptions({ socketOption, intervalOption, quitAfterOption, screenshotOption });
    parser.process(app);

    TrayClient client(parser.value(socketOption));
    TrayWindow window(client);

    QSystemTrayIcon tray;
    QMenu menu;
    QAction *summary = menu.addAction(QStringLiteral("Waiting for the daemon"));
    summary->setEnabled(false);
    menu.addSeparator();
    // Mode entries are inserted in front of this separator as they arrive.
    QAction *modeAnchor = menu.addSeparator();
    QAction *consoleAction = menu.addAction(QStringLiteral("Console"));
    QAction *quitAction = menu.addAction(QStringLiteral("Quit"));
    QActionGroup modeGroup(&menu);
    modeGroup.setExclusive(true);

    // Kept so the icon can be redrawn when the desktop changes its icon theme,
    // which is not a moment the daemon says anything.
    TrayClient::Status lastStatus;

    tray.setToolTip(QStringLiteral("Ombra"));
    tray.setIcon(trayIcon(false, false, -1, 0));
    tray.setContextMenu(&menu);
    // Not shown yet: the tray host registers over the session bus and may not
    // be up at login. Showing into a watcher that does not exist is how an icon
    // ends up missing for the whole session.

    const auto toggleConsole = [&window]() {
        if (window.isVisible()) {
            window.hide();
            return;
        }
        window.show();
        window.raise();
        window.activateWindow();
    };
    QObject::connect(consoleAction, &QAction::triggered, &window, toggleConsole);
    QObject::connect(&tray, &QSystemTrayIcon::activated, &window,
                     [toggleConsole](QSystemTrayIcon::ActivationReason reason) {
                         if (reason == QSystemTrayIcon::Trigger)
                             toggleConsole();
                     });
    QObject::connect(quitAction, &QAction::triggered, &app, &QApplication::quit);

    // One console per session. Two of them put two icons in the panel and two
    // pollers on the daemon, and nothing tells you which icon you clicked. A
    // second start therefore opens the first one's window instead: that is what
    // somebody clicking the launcher again is asking for anyway.
    //
    // Not in --screenshot mode. That run is a photograph taken beside whatever
    // is already in the panel, and it must not turn into "asked the running one
    // to open its window".
    QLocalServer guard;
    if (!parser.isSet(screenshotOption)) {
        QLocalSocket probe;
        probe.connectToServer(guardPath());
        if (probe.waitForConnected(kGuardTimeoutMs)) {
            probe.write("show\n");
            probe.flush();
            probe.waitForBytesWritten(kGuardTimeoutMs);
            qInfo().noquote() << "already running; asked it to open the console";
            return 0;
        }
        // Nobody answered, so any socket at that path is left over from a
        // process that is gone. Removing it is what makes a crash recoverable
        // without a reboot.
        QLocalServer::removeServer(guardPath());
        if (!guard.listen(guardPath()))
            qWarning().noquote() << "cannot hold the single-instance lock:" << guard.errorString();
        QObject::connect(&guard, &QLocalServer::newConnection, &window, [&guard, &window]() {
            while (QLocalSocket *client = guard.nextPendingConnection()) {
                QObject::connect(client, &QLocalSocket::disconnected, client,
                                 &QLocalSocket::deleteLater);
                window.show();
                window.raise();
                window.activateWindow();
            }
        });
    }

    QStringList shownModes;
    QObject::connect(&client, &TrayClient::statusChanged, &app,
                     [&](const TrayClient::Status &status) {
                         if (shownModes != status.modeIds) {
                             shownModes = status.modeIds;
                             rebuildModeActions(menu, modeAnchor, modeGroup, client, status);
                         }
                         for (QAction *action : modeGroup.actions())
                             action->setChecked(action->data().toString() == status.modeId);
                         summary->setText(summaryText(status));
                         lastStatus = status;
                         // The same line the menu shows: hovering the panel is
                         // the cheapest way to ask what is running, and a
                         // picture cannot spell out a mode's name.
                         tray.setToolTip(QStringLiteral("Ombra — ") + summaryText(status));
                         tray.setIcon(trayIcon(true, status.guardEngaged,
                                               status.modeIds.indexOf(status.modeId),
                                               status.modeIds.size()));
                     });
    QObject::connect(&client, &TrayClient::connectionChanged, &app, [&](bool connected) {
        if (!connected) {
            summary->setText(QStringLiteral("The daemon is not answering"));
            tray.setToolTip(QStringLiteral("Ombra — the daemon is not answering"));
            tray.setIcon(trayIcon(false, false, -1, 0));
        }
    });

    // The console's palette: the desktop's own colours where the desktop wrote
    // them for Qt, and re-read whenever it says its appearance moved. That signal
    // is what makes an automatic switch at sunset reach a console that has been
    // open all day — nothing else would, because a platform theme does not update
    // a running application's palette and reopening the window does not restart
    // the process.
    //
    // The panel icon is not part of this: it is drawn from constants, because a
    // panel is coloured by the desktop and not by this application's palette.
    theme::applyPreference();
    theme::watch(&app, [] { theme::applyPreference(); });

    if (parser.isSet(screenshotOption)) {
        // With real readings if the daemon is there: a console shown full of
        // dashes would document the disconnected case and nothing else.
        client.poll();
        if (!client.connected())
            qWarning() << "the daemon is not answering; the pictures will show that";
        QString error;
        if (!saveScreenshots(parser.value(screenshotOption), window, menu, &error)) {
            qCritical().noquote() << error;
            return 1;
        }
        return 0;
    }

    // The tray host is not there the moment a session starts. It comes up over
    // the session bus, asynchronously, and usually a second or two after the
    // processes an autostart entry launches — so asking once, at startup, gets
    // the answer "no tray" for a tray that is merely late, and the console then
    // opens by itself at every login.
    //
    // Until an icon exists, closing the console ends the process. Otherwise it
    // would live on with no window and no icon: invisible, unreachable, and
    // killable only by name.
    const int absentMs = qEnvironmentVariableIntValue("OMBRA_TEST_TRAY_ABSENT_MS");
    const int trayTimeoutMs = qEnvironmentVariableIsSet("OMBRA_TEST_TRAY_TIMEOUT_MS")
            ? qEnvironmentVariableIntValue("OMBRA_TEST_TRAY_TIMEOUT_MS")
            : kTrayWaitMs;
    auto *trayWait = new QTimer(&app);
    trayWait->setInterval(kTrayPollMs);
    // `waited` is captured by value into a mutable lambda rather than kept
    // beside the timer: it belongs to the timer, which outlives every scope
    // here except the application's own.
    QObject::connect(trayWait, &QTimer::timeout, &app,
                     [&app, &tray, &window, toggleConsole, trayWait, absentMs,
                      trayTimeoutMs, waited = 0]() mutable {
                         waited += kTrayPollMs;
                         // The first hook pretends the tray is missing for a
                         // while. Without it the "appeared late" branch cannot
                         // be reached at all: on the offscreen platform the
                         // availability check answers true from the first tick.
                         if (waited >= absentMs && QSystemTrayIcon::isSystemTrayAvailable()) {
                             trayWait->stop();
                             tray.show();
                             // There is somewhere to go back to now.
                             app.setQuitOnLastWindowClosed(false);
                             return;
                         }
                         if (waited < trayTimeoutMs)
                             return;
                         trayWait->stop();
                         qWarning().noquote()
                                 << "no system tray after" << trayTimeoutMs / 1000
                                 << "seconds; showing the console instead";
                         if (!window.isVisible())
                             toggleConsole();
                     });
    trayWait->start();

    const int quitAfterMs = parser.value(quitAfterOption).toInt();
    if (quitAfterMs > 0)
        QTimer::singleShot(quitAfterMs, &app, &QApplication::quit);

    const int intervalMs = parser.value(intervalOption).toInt();
    if (intervalMs > 0)
        client.start(intervalMs);
    else
        client.start();
    return app.exec();
}
