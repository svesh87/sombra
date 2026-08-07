// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "signal_pipe.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QSignalSpy>
#include <QTest>

#include <csignal>

// The signal path is the exit path that matters most: it is how systemd stops
// the daemon, and it is what has to end with the fan back on EC control.
class SignalPipeTest : public QObject {
    Q_OBJECT

private slots:
    void deliversTerminationIntoTheEventLoop();
    void deliversHangupSeparately();

private:
    static bool pump(QSignalSpy &spy, int wanted, int timeoutMs = 3000);
};

bool SignalPipeTest::pump(QSignalSpy &spy, int wanted, int timeoutMs)
{
    QDeadlineTimer deadline(timeoutMs);
    while (spy.count() < wanted && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return spy.count() >= wanted;
}

void SignalPipeTest::deliversTerminationIntoTheEventLoop()
{
    SignalPipe pipe;
    QVERIFY(pipe.install());
    QSignalSpy spy(&pipe, &SignalPipe::signalled);

    // Without the handler installed this would kill the test process, which is
    // itself the assertion that the handler took effect.
    ::raise(SIGTERM);
    QVERIFY(pump(spy, 1));
    QCOMPARE(spy.first().first().toInt(), SIGTERM);

    ::raise(SIGINT);
    QVERIFY(pump(spy, 2));
    QCOMPARE(spy.at(1).first().toInt(), SIGINT);
}

void SignalPipeTest::deliversHangupSeparately()
{
    SignalPipe pipe;
    QVERIFY(pipe.install());
    QSignalSpy spy(&pipe, &SignalPipe::signalled);

    ::raise(SIGHUP);
    QVERIFY(pump(spy, 1));
    QCOMPARE(spy.first().first().toInt(), SIGHUP);
}

QTEST_MAIN(SignalPipeTest)
#include "signal_pipe_test.moc"
