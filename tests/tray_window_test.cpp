// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "autostart.h"
#include "control_socket.h"
#include "fake_sysfs.h"
#include "modes.h"
#include "tray_window.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QLabel>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

namespace {

// The daemon runs as its own process here, which is what it is on the machine
// as well. It cannot be an object in this process: the window drives it
// through a blocking exchange, and a daemon sharing the thread would never
// reach its own event loop to answer.
class TestDaemon {
public:
    TestDaemon(const QString &sysfsRoot, QString workDir)
        : m_work(std::move(workDir))
    {
        m_process.start(binary(),
                        { QStringLiteral("--no-dbus"), QStringLiteral("--root"), sysfsRoot,
                          QStringLiteral("--ring"), m_work + QStringLiteral("/metrics.ring"),
                          QStringLiteral("--socket"), socketPath(), QStringLiteral("--state"),
                          m_work + QStringLiteral("/state"), QStringLiteral("--config"),
                          m_work + QStringLiteral("/modes.conf"), QStringLiteral("--capacity"),
                          QStringLiteral("64"), QStringLiteral("--interval-ms"),
                          QStringLiteral("20") });
    }

    ~TestDaemon() { stop(); }

    QString socketPath() const { return m_work + QStringLiteral("/control.sock"); }

    bool waitUntilListening()
    {
        if (!m_process.waitForStarted(10000))
            return false;
        QDeadlineTimer deadline(10000);
        while (!QFile::exists(socketPath()) && !deadline.hasExpired())
            QTest::qWait(10);
        return QFile::exists(socketPath());
    }

    // SIGTERM is how systemd stops it, and it restores the hardware and
    // removes the socket on the way out.
    void stop()
    {
        if (m_process.state() == QProcess::NotRunning)
            return;
        m_process.terminate();
        m_process.waitForFinished(10000);
    }

    static QString binary()
    {
        return QDir::cleanPath(QCoreApplication::applicationDirPath()
                               + QStringLiteral("/../ombrad"));
    }

private:
    QString m_work;
    QProcess m_process;
};

// The environment a spawned ombra-tray gets.
//
// The offscreen platform keeps its window off the display, but that is not
// enough on its own: QSystemTrayIcon speaks StatusNotifierItem over the session
// bus, which is a protocol and not a window, so an offscreen process still puts
// a real icon into the panel of whoever is running the suite — and a killed one
// never gets to remove it again. Pointing the process at a bus that does not
// exist leaves it with no tray to register with, and main() then shows the
// console instead, which offscreen means nothing at all.
QProcessEnvironment isolatedEnvironment()
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
                       QStringLiteral("unix:path=/nonexistent/ombra-test-has-no-session-bus"));
    return environment;
}

// A failing QVERIFY returns from the test, so a spawned process has to be
// reaped by a destructor: a line at the end of the case would be skipped, and
// the leftover would poll a dead socket for as long as the machine is up.
class Reaped {
public:
    explicit Reaped(QProcess &process)
        : m_process(process)
    {
    }
    ~Reaped()
    {
        if (m_process.state() == QProcess::NotRunning)
            return;
        m_process.kill();
        m_process.waitForFinished(10000);
    }

private:
    QProcess &m_process;
};

TrayClient::Status statusFrom(const QByteArray &json)
{
    return TrayClient::parseStatus(QJsonDocument::fromJson(json).object());
}

} // namespace

class TrayWindowTest : public QObject {
    Q_OBJECT

private slots:
    void init();

    void showsTheReadingsIncludingTheCorePowerSplit();
    void showsAbsentReadingsAsADash();
    void showsTheGuardAndTheWriteMismatch();
    void movingASliderTunesTheDaemonImmediately();
    void theOtherLiveControlsReachTheDaemon();
    void selectingAModeSendsTheModeCommand();
    void theMarkButtonsStampTheCurrentSample();
    void copiesTheLiveSetpointsAsAConfigurationSnippet();
    void maximumFanSpeedIsAControlOfItsOwn();
    void theBacklightSwitchIsOneAction();
    void theBacklightWindowIsTunedFromTheWindow();
    void theAutostartSwitchOwnsItsOwnEntry();
    void repaintingFromAStatusSendsNothing();
    void saysSoWhenTheDaemonIsUnreachable();
    void theTrayBinaryRunsAgainstADaemon();
    void theTrayBinaryWritesTheDocumentationScreenshots();

private:
    QString socketPath() const { return m_work.path() + QStringLiteral("/control.sock"); }
    QString configPath() const { return m_work.path() + QStringLiteral("/modes.conf"); }

    QTemporaryDir m_work;
    // Rebuilt before every test: a shared tree would carry one case's writes
    // into the next.
    std::unique_ptr<FakeSysfs> m_tree;
};

void TrayWindowTest::init()
{
    m_tree = std::make_unique<FakeSysfs>();
    QFile::remove(m_work.path() + QStringLiteral("/state"));
    QFile::remove(m_work.path() + QStringLiteral("/metrics.ring"));
}

void TrayWindowTest::showsTheReadingsIncludingTheCorePowerSplit()
{
    TrayClient client(socketPath());
    TrayWindow window(client);

    window.applyStatus(statusFrom(R"({
        "mode": "work",
        "modes": [{"id": "quiet", "label": "Quiet"}, {"id": "work", "label": "Work"}],
        "live_tuning": true, "on_ac": true,
        "applied": {"profile": "balanced", "pl1_w": 20, "pl2_w": 30, "max_perf_pct": 50,
                    "turbo": false, "epp": "power", "hwp_boost": false, "gt_max_mhz": 2350,
                    "fan_mode": 2},
        "requested": {"profile": "quiet", "pl1_w": 17, "pl2_w": 26, "max_perf_pct": 40,
                      "turbo": true, "epp": "performance", "hwp_boost": true,
                      "gt_max_mhz": 1200},
        "sensors": {"pkg_c": 61.0, "board_c": 45.0, "nvme_c": 38.8, "fan_rpm": 1900,
                    "pkg_w": 7.25, "core_w": 4.5, "uncore_w": 1.25, "cpu_mhz": 2400,
                    "gpu_mhz": 300, "load1": 0.61}
    })"));

    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("pkg_c"))->text(),
             QStringLiteral("61.0 °C"));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("board_c"))->text(),
             QStringLiteral("45.0 °C"));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("nvme_c"))->text(),
             QStringLiteral("38.8 °C"));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("fan_rpm"))->text(),
             QStringLiteral("1900 RPM"));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("pkg_w"))->text(),
             QStringLiteral("7.25 W"));
    // The split is what distinguishes CPU heat from iGPU heat.
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("core_w"))->text(),
             QStringLiteral("4.50 W"));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("uncore_w"))->text(),
             QStringLiteral("1.25 W"));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("cpu_mhz"))->text(),
             QStringLiteral("2400 MHz"));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("gpu_mhz"))->text(),
             QStringLiteral("300 MHz"));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("load1"))->text(),
             QStringLiteral("0.61"));

    // The sliders follow what was asked for; what actually reached the
    // hardware is stated separately, because the guard can differ from it.
    QCOMPARE(window.findChild<QSlider *>(QStringLiteral("pl1"))->value(), 17);
    QCOMPARE(window.findChild<QSlider *>(QStringLiteral("pl2"))->value(), 26);
    QCOMPARE(window.findChild<QSlider *>(QStringLiteral("max_perf_pct"))->value(), 40);
    QCOMPARE(window.findChild<QSlider *>(QStringLiteral("gt_max_mhz"))->value(), 1200);
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("pl1_value"))->text(),
             QStringLiteral("17 W"));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("gt_max_mhz_value"))->text(),
             QStringLiteral("1200 MHz"));
    QCOMPARE(window.findChild<QComboBox *>(QStringLiteral("epp"))->currentText(),
             QStringLiteral("performance"));
    QCOMPARE(window.findChild<QComboBox *>(QStringLiteral("profile"))->currentText(),
             QStringLiteral("quiet"));
    QVERIFY(window.findChild<QCheckBox *>(QStringLiteral("turbo"))->isChecked());
    QVERIFY(window.findChild<QCheckBox *>(QStringLiteral("hwp_boost"))->isChecked());
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("applied"))->text()
                    .contains(QStringLiteral("PL1 20 W")));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("power_source"))->text(),
             QStringLiteral("On mains power"));
    // Live tuning is announced, because those values survive nothing.
    QVERIFY(!window.findChild<QLabel *>(QStringLiteral("live_tuning"))->isHidden());

    auto *mode = window.findChild<QComboBox *>(QStringLiteral("mode"));
    QCOMPARE(mode->count(), 2);
    QCOMPARE(mode->currentText(), QStringLiteral("Work"));
    QCOMPARE(mode->currentData().toString(), QStringLiteral("work"));
}

void TrayWindowTest::showsAbsentReadingsAsADash()
{
    TrayClient client(socketPath());
    TrayWindow window(client);

    window.applyStatus(statusFrom(R"({
        "mode": "work", "modes": [{"id": "work", "label": "Work"}],
        "applied": {"pl1_w": 20, "fan_mode": 2}, "requested": {"pl1_w": 20},
        "sensors": {"pkg_c": null, "board_c": null, "nvme_c": null, "fan_rpm": null,
                    "pkg_w": null, "core_w": null, "uncore_w": null, "cpu_mhz": null,
                    "gpu_mhz": null, "load1": null}
    })"));

    // A dash, never an invented zero: zero is a legal fan speed and a legal
    // power draw.
    for (const QString &name : { QStringLiteral("pkg_c"), QStringLiteral("board_c"),
                                 QStringLiteral("nvme_c"), QStringLiteral("fan_rpm"),
                                 QStringLiteral("pkg_w"), QStringLiteral("core_w"),
                                 QStringLiteral("uncore_w"), QStringLiteral("cpu_mhz"),
                                 QStringLiteral("gpu_mhz"), QStringLiteral("load1") }) {
        QCOMPARE(window.findChild<QLabel *>(name)->text(), QStringLiteral("—"));
    }
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("power_source"))->text(),
             QStringLiteral("On battery"));
}

void TrayWindowTest::showsTheGuardAndTheWriteMismatch()
{
    TrayClient client(socketPath());
    TrayWindow window(client);
    auto *guard = window.findChild<QLabel *>(QStringLiteral("guard"));
    auto *mismatch = window.findChild<QLabel *>(QStringLiteral("mismatch"));
    auto *sensors = window.findChild<QLabel *>(QStringLiteral("sensors"));
    auto *live = window.findChild<QLabel *>(QStringLiteral("live_tuning"));
    QVERIFY(guard->isHidden());
    QVERIFY(mismatch->isHidden());

    window.applyStatus(statusFrom(R"({
        "mode": "full", "modes": [{"id": "full", "label": "Full"}],
        "guard_engaged": true, "write_mismatch": true, "sensors_degraded": true,
        "live_tuning": false,
        "applied": {"pl1_w": 10, "pl2_w": 15, "fan_mode": 0},
        "requested": {"pl1_w": 45, "pl2_w": 65},
        "sensors": {"pkg_c": 99.5}
    })"));

    // The guard is a safety interlock and its state has to be visible.
    QVERIFY(!guard->isHidden());
    // A value the daemon did not write means something else is writing these
    // controls too, which is the one thing this design forbids.
    QVERIFY(!mismatch->isHidden());
    QVERIFY(!sensors->isHidden());
    QVERIFY(live->isHidden());
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("applied"))->text()
                    .contains(QStringLiteral("PL1 10 W")));
    QCOMPARE(window.findChild<QSlider *>(QStringLiteral("pl1"))->value(), 45);
    // The window reports the fan that is actually running, and the guard is
    // the one part of automatic operation that forces it.
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("fan_state"))->text(),
             QStringLiteral("Fan: full speed"));

    window.applyStatus(statusFrom(R"({
        "mode": "full", "modes": [{"id": "full", "label": "Full"}],
        "applied": {"pl1_w": 45, "fan_mode": 2}, "requested": {"pl1_w": 45},
        "sensors": {"pkg_c": 61.0}
    })"));
    QVERIFY(guard->isHidden());
    QVERIFY(mismatch->isHidden());
    QVERIFY(sensors->isHidden());
}

void TrayWindowTest::movingASliderTunesTheDaemonImmediately()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();

    auto *pl1 = window.findChild<QSlider *>(QStringLiteral("pl1"));
    QCOMPARE(pl1->value(), 20);
    // Nothing, not a slider either, raises a limit past what the maintainer
    // has confirmed on this machine.
    QCOMPARE(pl1->maximum(), kMaxConfirmedPl1W);

    pl1->setValue(15);
    QVERIFY(client.status().liveTuning);
    QCOMPARE(client.status().requested.pl1W, 15);
    // The rest of the live overlay travels with it, unchanged.
    QCOMPARE(client.status().requested.pl2W, 30);
    QCOMPARE(client.status().requested.maxPerfPct, 50);
    QCOMPARE(client.status().requested.profile, Profile::Balanced);
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("pl1_value"))->text(),
             QStringLiteral("15 W"));
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("notice"))->text()
                    .contains(QStringLiteral("pl1=15")));
    // Applied at once and never written back: the configuration file is the
    // single source of truth and the tray does not touch it.
    QVERIFY(!QFile::exists(configPath()));

    window.findChild<QPushButton *>(QStringLiteral("reset"))->click();
    QVERIFY(!client.status().liveTuning);
    QCOMPARE(client.status().requested.pl1W, 20);
    QCOMPARE(pl1->value(), 20);
}

void TrayWindowTest::theOtherLiveControlsReachTheDaemon()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();

    window.findChild<QSlider *>(QStringLiteral("pl2"))->setValue(40);
    QCOMPARE(client.status().requested.pl2W, 40);
    window.findChild<QSlider *>(QStringLiteral("max_perf_pct"))->setValue(35);
    QCOMPARE(client.status().requested.maxPerfPct, 35);
    // The iGPU ceiling exists because none of the CPU-side controls touch the
    // graphics engine, and a video call loads it as heavily as the cores.
    window.findChild<QSlider *>(QStringLiteral("gt_max_mhz"))->setValue(1200);
    QCOMPARE(client.status().requested.gtMaxMhz, 1200);
    window.findChild<QCheckBox *>(QStringLiteral("turbo"))->setChecked(true);
    QVERIFY(!client.status().requested.noTurbo);
    window.findChild<QCheckBox *>(QStringLiteral("hwp_boost"))->setChecked(true);
    QVERIFY(client.status().requested.hwpBoost);

    auto *epp = window.findChild<QComboBox *>(QStringLiteral("epp"));
    epp->setCurrentIndex(epp->findData(static_cast<int>(Epp::BalancePower)));
    QCOMPARE(client.status().requested.epp, Epp::BalancePower);

    // The platform profile is the EC fan curve, which is the noise-versus-
    // temperature dial and one of the things worth judging by ear.
    auto *profile = window.findChild<QComboBox *>(QStringLiteral("profile"));
    profile->setCurrentIndex(profile->findData(static_cast<int>(Profile::Quiet)));
    QCOMPARE(client.status().requested.profile, Profile::Quiet);
    QTRY_COMPARE(m_tree->read(FakeSysfs::platformProfile()), QStringLiteral("quiet"));
}

void TrayWindowTest::selectingAModeSendsTheModeCommand()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    QSignalSpy selected(&window, &TrayWindow::modeSelected);
    client.poll();

    auto *mode = window.findChild<QComboBox *>(QStringLiteral("mode"));
    QCOMPARE(mode->count(), 3);
    // Ordered coolest first, with the working mode in the middle.
    QCOMPARE(mode->itemData(0).toString(), QStringLiteral("quiet"));
    QCOMPARE(mode->currentIndex(), 1);
    QCOMPARE(selected.count(), 0);

    mode->setCurrentIndex(0);
    QCOMPARE(selected.count(), 1);
    QCOMPARE(selected.first().first().toString(), QStringLiteral("quiet"));
    QCOMPARE(client.status().modeId, QStringLiteral("quiet"));
    QCOMPARE(mode->currentIndex(), 0);

    // Selecting a mode is the deliberate act that drops live tuning.
    window.findChild<QSlider *>(QStringLiteral("pl1"))->setValue(11);
    QVERIFY(client.status().liveTuning);
    mode->setCurrentIndex(2);
    QVERIFY(!client.status().liveTuning);
    QCOMPARE(client.status().modeId, QStringLiteral("full"));
}

void TrayWindowTest::theMarkButtonsStampTheCurrentSample()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    // The verdict buttons are dead until something answers, so the window has
    // to have seen a status before they can be clicked.
    client.poll();
    auto *notice = window.findChild<QLabel *>(QStringLiteral("notice"));

    // A judgement made by ear is worthless unless it can be traced back to the
    // numbers that produced it, which is what stamping the sample buys.
    window.findChild<QPushButton *>(QStringLiteral("mark_good"))->click();
    QCOMPARE(notice->text(), QStringLiteral("mark good → ok"));
    window.findChild<QPushButton *>(QStringLiteral("mark_too_loud"))->click();
    QCOMPARE(notice->text(), QStringLiteral("mark too-loud → ok"));
    window.findChild<QPushButton *>(QStringLiteral("mark_too_slow"))->click();
    QCOMPARE(notice->text(), QStringLiteral("mark too-slow → ok"));
}

void TrayWindowTest::copiesTheLiveSetpointsAsAConfigurationSnippet()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();
    window.findChild<QSlider *>(QStringLiteral("pl1"))->setValue(17);

    QGuiApplication::clipboard()->clear();
    window.findChild<QPushButton *>(QStringLiteral("copy_snippet"))->click();

    // The tray never writes the configuration; promoting a live value into it
    // stays a deliberate edit by the maintainer.
    const QString snippet = QGuiApplication::clipboard()->text();
    QVERIFY(snippet.contains(QStringLiteral("[mode work]")));
    QVERIFY(snippet.contains(QStringLiteral("pl1_w=17")));
    QVERIFY(snippet.contains(QStringLiteral("platform_profile=balanced")));
    QVERIFY(!QFile::exists(configPath()));
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("notice"))->text()
                    .contains(QStringLiteral("clipboard")));

    // The snippet is the configuration parser's own input, so it reads back.
    QFile file(m_work.path() + QStringLiteral("/snippet.conf"));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(snippet.toUtf8());
    file.close();
    ModeTable table;
    QVERIFY(table.load(file.fileName()));
    QCOMPARE(table.modes().size(), 1);
    QCOMPARE(table.modes().first().setpoints.pl1W, 17);
    QVERIFY(file.remove());
}

void TrayWindowTest::maximumFanSpeedIsAControlOfItsOwn()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();

    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("fan_state"))->text(),
             QStringLiteral("Fan: automatic, EC controlled"));
    QCOMPARE(m_tree->read(FakeSysfs::asus() + QStringLiteral("/pwm1_enable")),
             QStringLiteral("2"));

    // Full speed is reachable only on explicit demand and is independent of
    // the mode; automatic control never engages it.
    window.findChild<QPushButton *>(QStringLiteral("fan_max"))->click();
    QTRY_COMPARE(m_tree->read(FakeSysfs::asus() + QStringLiteral("/pwm1_enable")),
                 QStringLiteral("0"));
    QCOMPARE(client.status().modeId, QStringLiteral("work"));
    client.poll();
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("fan_state"))->text(),
             QStringLiteral("Fan: full speed"));

    window.findChild<QPushButton *>(QStringLiteral("fan_auto"))->click();
    QTRY_COMPARE(m_tree->read(FakeSysfs::asus() + QStringLiteral("/pwm1_enable")),
                 QStringLiteral("2"));
}

void TrayWindowTest::theBacklightSwitchIsOneAction()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();

    auto *toggle = window.findChild<QCheckBox *>(QStringLiteral("auto_brightness"));
    auto *profile = window.findChild<QComboBox *>(QStringLiteral("brightness_profile"));
    QVERIFY(toggle->isEnabled());
    // Off by default: the automation is something the person switches on.
    QVERIFY(!toggle->isChecked());
    QCOMPARE(profile->count(), 1);
    QCOMPARE(profile->itemData(0).toString(), QStringLiteral("indoors"));

    // One click, and the daemon has it.
    toggle->setChecked(true);
    QVERIFY(client.status().autoBrightness);
    toggle->setChecked(false);
    QVERIFY(!client.status().autoBrightness);

    // The selector is its own control, and it is not the thermal mode's.
    profile->setCurrentIndex(0);
    QCOMPARE(client.status().brightnessProfileId, QStringLiteral("indoors"));
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("brightness_state"))
                    ->text()
                    .contains(QStringLiteral("your level")));
}

void TrayWindowTest::theBacklightWindowIsTunedFromTheWindow()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();

    auto *span = window.findChild<QSlider *>(QStringLiteral("brightness_span"));
    auto *floor = window.findChild<QSlider *>(QStringLiteral("brightness_floor"));
    auto *ceiling = window.findChild<QSlider *>(QStringLiteral("brightness_ceiling"));
    QVERIFY(span->isEnabled());
    QCOMPARE(span->value(), kBrightnessDefaultSpanPct);

    // "The range is too narrow" is answered here rather than by editing the
    // file, and the answer applies at once.
    span->setValue(30);
    QTRY_COMPARE(client.status().brightnessSpanPct, 30);
    QVERIFY(client.status().brightnessLive);
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("brightness_span_value"))->text(),
             QStringLiteral("\u00B1 30 pp"));

    // The pair cannot cross: the dragged slider pushes the other ahead of it
    // rather than reaching a pair the daemon refuses.
    ceiling->setValue(60);
    QTRY_COMPARE(client.status().brightnessCeilingPct, 60);
    floor->setValue(80);
    QTRY_COMPARE(client.status().brightnessCeilingPct, 80);
    QCOMPARE(client.status().brightnessFloorPct, 80);
    floor->setValue(20);
    ceiling->setValue(10);
    QTRY_COMPARE(client.status().brightnessFloorPct, 10);

    // Nothing was written back, so dropping the overlay returns the profile's
    // own numbers.
    window.findChild<QPushButton *>(QStringLiteral("brightness_reset"))->click();
    QTRY_VERIFY(!client.status().brightnessLive);
    QCOMPARE(client.status().brightnessSpanPct, kBrightnessDefaultSpanPct);
}

void TrayWindowTest::theAutostartSwitchOwnsItsOwnEntry()
{
    QTemporaryDir config;
    QVERIFY(config.isValid());
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());

    // No daemon on purpose: the tray starting with the session has nothing to
    // do with whether the daemon is answering, so the switch must work anyway.
    TrayClient client(socketPath());
    TrayWindow window(client);
    auto *toggle = window.findChild<QCheckBox *>(QStringLiteral("autostart"));
    QVERIFY(toggle);
    QVERIFY(toggle->isEnabled());
    QVERIFY(!toggle->isChecked());

    toggle->setChecked(true);
    QVERIFY(QFile::exists(autostart::entryPath()));
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("notice"))
                    ->text()
                    .contains(QStringLiteral("next login")));

    toggle->setChecked(false);
    QVERIFY(!QFile::exists(autostart::entryPath()));

    // An entry already there is what a freshly opened window reads, because the
    // file is the state and nothing else is remembered.
    QVERIFY(autostart::setEnabled(true));
    TrayWindow reopened(client);
    QVERIFY(reopened.findChild<QCheckBox *>(QStringLiteral("autostart"))->isChecked());
    QVERIFY(autostart::setEnabled(false));

    // A failed write puts the switch back: a checkbox that stayed on would be
    // claiming something the file does not say.
    qputenv("XDG_CONFIG_HOME", QByteArray("/proc/self/impossible"));
    toggle->setChecked(true);
    QVERIFY(!toggle->isChecked());
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("notice"))
                    ->text()
                    .startsWith(QStringLiteral("autostart")));

    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
}

void TrayWindowTest::repaintingFromAStatusSendsNothing()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();

    // Moving the controls to match a status update must not look like the user
    // moving them, or the tray would tune the daemon towards its own display.
    window.applyStatus(statusFrom(R"({
        "mode": "full",
        "modes": [{"id": "quiet", "label": "Quiet"}, {"id": "work", "label": "Work"},
                  {"id": "full", "label": "Full"}],
        "applied": {"profile": "performance", "pl1_w": 33, "fan_mode": 0},
        "requested": {"profile": "performance", "pl1_w": 33, "pl2_w": 44,
                      "max_perf_pct": 90, "turbo": true, "epp": "performance",
                      "hwp_boost": true, "gt_max_mhz": 1500},
        "sensors": {"pkg_c": 61.0}
    })"));
    QCOMPARE(window.findChild<QSlider *>(QStringLiteral("pl1"))->value(), 33);
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("fan_state"))->text(),
             QStringLiteral("Fan: full speed"));
    QCOMPARE(window.findChild<QComboBox *>(QStringLiteral("mode"))->currentIndex(), 2);

    client.poll();
    QVERIFY(!client.status().liveTuning);
    QCOMPARE(client.status().modeId, QStringLiteral("work"));
    QCOMPARE(client.status().requested.pl1W, 20);
    QCOMPARE(client.status().applied.fanMode, 2);
}

void TrayWindowTest::saysSoWhenTheDaemonIsUnreachable()
{
    TrayClient client(m_work.path() + QStringLiteral("/absent.sock"));
    TrayWindow window(client);

    // The banner names the socket and the group, because "not answering" on
    // its own leaves the person guessing which of the two usual causes it is.
    auto *banner = window.findChild<QLabel *>(QStringLiteral("connection"));
    QVERIFY(!banner->isHidden());
    QVERIFY(banner->text().contains(QStringLiteral("not answering")));
    QVERIFY(banner->text().contains(client.socketPath()));
    QVERIFY(banner->text().contains(QString::fromLatin1(kControlSocketGroup)));

    // Nothing that would send a command is usable, so a slider parked at its
    // minimum cannot be read as a setpoint the daemon is holding.
    QVERIFY(!window.findChild<QSlider *>(QStringLiteral("pl1"))->isEnabled());
    QVERIFY(!window.findChild<QComboBox *>(QStringLiteral("mode"))->isEnabled());
    QVERIFY(!window.findChild<QCheckBox *>(QStringLiteral("auto_brightness"))->isEnabled());

    // A command that goes nowhere says so rather than looking like it worked.
    window.markGood();
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("notice"))->text(),
             QStringLiteral("mark good → no reply"));
    window.copyConfigSnippet();
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("notice"))->text(),
             QStringLiteral("snippet → no reply"));

    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient live(socketPath());
    TrayWindow second(live);
    auto *connection = second.findChild<QLabel *>(QStringLiteral("connection"));
    QVERIFY(connection->text().contains(QStringLiteral("not answering")));
    live.poll();
    QVERIFY(connection->text().contains(QStringLiteral("Connected")));
    QVERIFY(connection->text().contains(live.socketPath()));
    QVERIFY(second.findChild<QSlider *>(QStringLiteral("pl1"))->isEnabled());
}

void TrayWindowTest::theTrayBinaryRunsAgainstADaemon()
{
    const QString binary = QDir::cleanPath(QCoreApplication::applicationDirPath()
                                           + QStringLiteral("/../ombra-tray"));
    QVERIFY2(QFile::exists(binary), qPrintable(binary));

    auto daemon = std::make_unique<TestDaemon>(m_tree->root(), m_work.path());
    QVERIFY(daemon->waitUntilListening());

    QProcess tray;
    const Reaped reaped(tray);
    tray.setProcessEnvironment(isolatedEnvironment());
    tray.start(binary, { QStringLiteral("--socket"), socketPath(),
                         QStringLiteral("--interval-ms"), QStringLiteral("40"),
                         QStringLiteral("--quit-after-ms"), QStringLiteral("2000") });
    QVERIFY(tray.waitForStarted(10000));

    // Long enough to have polled, so the menu and the icon are built from a
    // real status reply rather than from the initial blank state.
    QTest::qWait(600);
    QCOMPARE(tray.state(), QProcess::Running);
    // And it survives the daemon going away underneath it.
    daemon.reset();
    QVERIFY(tray.waitForFinished(20000));
    QCOMPARE(tray.exitStatus(), QProcess::NormalExit);
    QCOMPARE(tray.exitCode(), 0);

    QProcess help;
    help.setProcessEnvironment(isolatedEnvironment());
    help.start(binary, { QStringLiteral("--help") });
    QVERIFY(help.waitForFinished(10000));
    QCOMPARE(help.exitCode(), 0);
    QVERIFY(!help.readAllStandardOutput().isEmpty());

    QProcess version;
    version.setProcessEnvironment(isolatedEnvironment());
    version.start(binary, { QStringLiteral("--version") });
    QVERIFY(version.waitForFinished(10000));
    QCOMPARE(version.exitCode(), 0);

    QProcess nonsense;
    nonsense.setProcessEnvironment(isolatedEnvironment());
    nonsense.start(binary, { QStringLiteral("--nonsense") });
    QVERIFY(nonsense.waitForFinished(10000));
    QCOMPARE(nonsense.exitCode(), 1);
}

void TrayWindowTest::theTrayBinaryWritesTheDocumentationScreenshots()
{
    const QString binary = QDir::cleanPath(QCoreApplication::applicationDirPath()
                                           + QStringLiteral("/../ombra-tray"));
    QVERIFY2(QFile::exists(binary), qPrintable(binary));

    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());

    QTemporaryDir target;
    QProcess shot;
    const Reaped reapedShot(shot);
    shot.setProcessEnvironment(isolatedEnvironment());
    shot.start(binary, { QStringLiteral("--socket"), socketPath(),
                         QStringLiteral("--screenshot"), target.path() });
    QVERIFY(shot.waitForFinished(30000));
    QCOMPARE(shot.exitCode(), 0);

    // The menu and one picture per tab, all readable as images: a documentation
    // command that writes an empty file is worse than one that fails.
    QStringList expected { QStringLiteral("tray-menu.png") };
    {
        TrayClient client(socketPath());
        const TrayWindow window(client);
        const auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs);
        QVERIFY(tabs->count() > 1);
        for (int i = 0; i < tabs->count(); ++i)
            expected.append(QStringLiteral("console-%1.png").arg(tabs->tabText(i).toLower()));
    }
    for (const QString &name : std::as_const(expected)) {
        const QImage picture(target.filePath(name));
        QVERIFY2(!picture.isNull(), qPrintable(name));
        QVERIFY2(picture.width() > 100 && picture.height() > 100, qPrintable(name));
    }

    // A directory that is not there is a failure with a reason, not a silent
    // success: the pictures would simply be missing from the next commit.
    QProcess nowhere;
    const Reaped reapedNowhere(nowhere);
    nowhere.setProcessEnvironment(isolatedEnvironment());
    nowhere.start(binary, { QStringLiteral("--socket"), socketPath(),
                            QStringLiteral("--screenshot"),
                            target.filePath(QStringLiteral("absent")) });
    QVERIFY(nowhere.waitForFinished(30000));
    QCOMPARE(nowhere.exitCode(), 1);
}

QTEST_MAIN(TrayWindowTest)
#include "tray_window_test.moc"
