// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QObject>
#include <QSocketNotifier>

#include <memory>

// Delivers POSIX signals into the Qt event loop.
//
// The handler itself may only write one byte to a pipe; everything that
// matters — restoring the fan to automatic control and the power limits to a
// confirmed-safe state — has to happen on the event loop, where it is ordinary
// code that can be tested.
class SignalPipe : public QObject {
    Q_OBJECT

public:
    explicit SignalPipe(QObject *parent = nullptr);
    ~SignalPipe() override;

    // Installs handlers for SIGTERM, SIGINT and SIGHUP. Returns false if the
    // pipe could not be created, in which case the daemon still runs but exits
    // without restoring — which is why the systemd unit also restores the fan
    // from ExecStopPost.
    bool install();

signals:
    void signalled(int number);

private slots:
    void onReadable();

private:
    int m_readFd = -1;
    std::unique_ptr<QSocketNotifier> m_notifier;
};
