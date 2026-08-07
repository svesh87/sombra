// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "signal_pipe.h"

#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

namespace {

int g_writeFd = -1;

extern "C" void forwardSignal(int number)
{
    if (g_writeFd < 0)
        return;
    const unsigned char byte = static_cast<unsigned char>(number);
    // Nothing else is async-signal-safe, and a failed write only means the
    // pipe is full, which already carries the same message.
    const ssize_t ignored = ::write(g_writeFd, &byte, 1);
    (void)ignored;
}

} // namespace

SignalPipe::SignalPipe(QObject *parent)
    : QObject(parent)
{
}

SignalPipe::~SignalPipe()
{
    m_notifier.reset();
    if (m_readFd >= 0)
        ::close(m_readFd);
    if (g_writeFd >= 0) {
        ::close(g_writeFd);
        g_writeFd = -1;
    }
}

bool SignalPipe::install()
{
    int fds[2] = { -1, -1 };
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
        return false;

    m_readFd = fds[0];
    g_writeFd = fds[1];

    m_notifier = std::make_unique<QSocketNotifier>(m_readFd, QSocketNotifier::Read, this);
    connect(m_notifier.get(), &QSocketNotifier::activated, this, &SignalPipe::onReadable);

    struct sigaction action {};
    action.sa_handler = forwardSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    ::sigaction(SIGTERM, &action, nullptr);
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGHUP, &action, nullptr);
    return true;
}

void SignalPipe::onReadable()
{
    unsigned char buffer[16];
    ssize_t got = ::read(m_readFd, buffer, sizeof(buffer));
    for (ssize_t i = 0; i < got; ++i)
        emit signalled(static_cast<int>(buffer[i]));
}
