// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

// The entry lives under XDG_CONFIG_HOME, which is what lets this run without
// touching the config directory of whoever is running the suite.
class AutostartTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void theEntryIsTheState();
    void theEntryIsADesktopFileThatStartsThisBinary();
    void switchingOffTwiceIsNotAnError();
    void anUnwritableLocationFailsWithAReason();

private:
    QTemporaryDir m_config;
};

void AutostartTest::initTestCase()
{
    QVERIFY(m_config.isValid());
    qputenv("XDG_CONFIG_HOME", m_config.path().toUtf8());
    QVERIFY(autostart::entryPath().startsWith(m_config.path()));
}

void AutostartTest::init()
{
    QFile::remove(autostart::entryPath());
}

void AutostartTest::theEntryIsTheState()
{
    // Nothing is remembered anywhere else, which is what makes "switched off and
    // it stayed off" a property of the arrangement rather than of a mechanism.
    QVERIFY(!autostart::enabled());
    QVERIFY(autostart::setEnabled(true));
    QVERIFY(autostart::enabled());
    QVERIFY(QFile::exists(autostart::entryPath()));

    QVERIFY(autostart::setEnabled(false));
    QVERIFY(!autostart::enabled());
    QVERIFY(!QFile::exists(autostart::entryPath()));

    // An entry put there by hand reads as on, because the file is the state.
    QDir().mkpath(QFileInfo(autostart::entryPath()).absolutePath());
    QFile written(autostart::entryPath());
    QVERIFY(written.open(QIODevice::WriteOnly));
    written.write("[Desktop Entry]\nType=Application\nExec=/usr/bin/sombra-tray\n");
    written.close();
    QVERIFY(autostart::enabled());
}

void AutostartTest::theEntryIsADesktopFileThatStartsThisBinary()
{
    QVERIFY(autostart::setEnabled(true));

    QSettings entry(autostart::entryPath(), QSettings::IniFormat);
    QCOMPARE(entry.status(), QSettings::NoError);
    entry.beginGroup(QStringLiteral("Desktop Entry"));
    QCOMPARE(entry.value(QStringLiteral("Type")).toString(), QStringLiteral("Application"));
    QCOMPARE(entry.value(QStringLiteral("Name")).toString(), QStringLiteral("Sombra"));
    // The running binary, so the entry starts the same tray the switch was
    // thrown in rather than whatever happens to be installed.
    QCOMPARE(entry.value(QStringLiteral("Exec")).toString(),
             QCoreApplication::applicationFilePath());
}

void AutostartTest::switchingOffTwiceIsNotAnError()
{
    QVERIFY(!autostart::enabled());
    QVERIFY(autostart::setEnabled(false));
    QVERIFY(!autostart::enabled());
}

void AutostartTest::anUnwritableLocationFailsWithAReason()
{
    // /proc is there and is not writable, so this exercises the reported
    // failure rather than a hypothetical one.
    qputenv("XDG_CONFIG_HOME", QByteArray("/proc/self/impossible"));
    QString error;
    QVERIFY(!autostart::setEnabled(true, &error));
    QVERIFY2(!error.isEmpty(), qPrintable(error));
    QVERIFY(!autostart::enabled());

    qputenv("XDG_CONFIG_HOME", m_config.path().toUtf8());
}

QTEST_MAIN(AutostartTest)
#include "autostart_test.moc"
