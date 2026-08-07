// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "tray_window.h"

#include "autostart.h"
#include "control_socket.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSlider>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {

// Room for a vertical scrollbar beside the widest page, in device-independent
// pixels, so that a page is never squeezed sideways to make room for one.
constexpr int kSideChromePx = 48;
// The same for the height the tab bar, the connection banner and the reply line
// take together, none of which is part of a page.
constexpr int kVerticalChromePx = 150;

// A reading the daemon could not take arrives as the sentinel. Rendering it as
// a dash rather than as a number is the whole point of having a sentinel: a
// zero here would be indistinguishable from a stopped fan.
QString formatted(double value, const QString &unit, int decimals)
{
    if (value <= TrayClient::kNoReading)
        return QStringLiteral("—");
    const QString number = QString::number(value, 'f', decimals);
    return unit.isEmpty() ? number : QStringLiteral("%1 %2").arg(number, unit);
}

QLabel *addReading(QFormLayout *form, const QString &caption, const QString &name)
{
    auto *label = new QLabel(QStringLiteral("—"), form->parentWidget());
    label->setObjectName(name);
    form->addRow(caption, label);
    return label;
}

// A slider that applies on the spot and is never written back anywhere.
// Deciding a setpoint is a judgement — by ear on the thermal side, by eye on
// the backlight one — and that judgement needs the machine to change while it
// is being made.
void addTuner(QFormLayout *form, const QString &caption, const QString &name, int minimum,
              int maximum, int step, QSlider **slider, QLabel **value)
{
    QWidget *parent = form->parentWidget();
    *slider = new QSlider(Qt::Horizontal, parent);
    (*slider)->setObjectName(name);
    (*slider)->setRange(minimum, maximum);
    (*slider)->setSingleStep(step);
    (*slider)->setPageStep(step * 5);
    *value = new QLabel(parent);
    (*value)->setObjectName(name + QStringLiteral("_value"));
    (*value)->setMinimumWidth(70);
    auto *row = new QWidget(parent);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->addWidget(*slider);
    rowLayout->addWidget(*value);
    form->addRow(caption, row);
}

QLabel *addIndicator(QVBoxLayout *layout, const QString &text, const QString &name)
{
    auto *label = new QLabel(text, layout->parentWidget());
    label->setObjectName(name);
    label->setVisible(false);
    layout->addWidget(label);
    return label;
}

} // namespace

class TrayWindow::Ui {
public:
    // The groups are split across tabs rather than stacked in one column. One
    // column of all of them is taller than the screen, and a window whose
    // bottom is off screen hides the controls at the end of it. Each tab is
    // still a scroll area, because a short screen can cut even one tab.
    QTabWidget *tabs = nullptr;
    // The widget inside each tab's scroll area, in tab order. Kept because the
    // opening size is the largest of them and nothing else can say what that
    // is before anything has been shown.
    QList<QWidget *> pages;
    // The groups that command the daemon, as opposed to the ones that only
    // report. Disabled while the daemon is not answering, so that sliders
    // sitting at their minimum cannot be read as setpoints the daemon holds.
    QList<QWidget *> commands;

    QComboBox *mode = nullptr;

    QLabel *packageC = nullptr;
    QLabel *boardC = nullptr;
    QLabel *nvmeC = nullptr;
    QLabel *fanRpm = nullptr;
    QLabel *packageW = nullptr;
    // The core/uncore split is what tells CPU heat from iGPU heat, and it is
    // the reason the two sub-domain readings exist at all.
    QLabel *coreW = nullptr;
    QLabel *uncoreW = nullptr;
    QLabel *cpuMhz = nullptr;
    QLabel *gpuMhz = nullptr;
    QLabel *load1 = nullptr;

    QLabel *applied = nullptr;
    QLabel *powerSource = nullptr;
    QLabel *connection = nullptr;
    QLabel *live = nullptr;
    QLabel *guard = nullptr;
    QLabel *mismatch = nullptr;
    QLabel *sensors = nullptr;
    QLabel *notice = nullptr;

    QSlider *pl1 = nullptr;
    QSlider *pl2 = nullptr;
    QSlider *maxPerfPct = nullptr;
    QSlider *gtMaxMhz = nullptr;
    QLabel *pl1Value = nullptr;
    QLabel *pl2Value = nullptr;
    QLabel *maxPerfPctValue = nullptr;
    QLabel *gtMaxMhzValue = nullptr;
    QComboBox *epp = nullptr;
    QComboBox *profile = nullptr;
    QCheckBox *turbo = nullptr;
    QCheckBox *hwpBoost = nullptr;
    QLabel *fanState = nullptr;

    QCheckBox *autoBrightness = nullptr;
    QComboBox *brightnessProfile = nullptr;
    QLabel *brightnessState = nullptr;
    QSlider *brightnessSpan = nullptr;
    QSlider *brightnessFloor = nullptr;
    QSlider *brightnessCeiling = nullptr;
    QLabel *brightnessSpanValue = nullptr;
    QLabel *brightnessFloorValue = nullptr;
    QLabel *brightnessCeilingValue = nullptr;

    // The one control that is about the console rather than about the machine,
    // and the one thing the tray writes. Never greyed out with the others: the
    // tray starting with the session does not depend on the daemon.
    QCheckBox *autostart = nullptr;
};

TrayWindow::TrayWindow(TrayClient &client, QWidget *parent)
    : QWidget(parent)
    , m_ui(new Ui)
    , m_client(client)
{
    setWindowTitle(QStringLiteral("Ombra"));
    build();

    connect(&m_client, &TrayClient::statusChanged, this, &TrayWindow::applyStatus);
    connect(&m_client, &TrayClient::connectionChanged, this, &TrayWindow::onConnectionChanged);
    onConnectionChanged(m_client.connected());

}

// Sized here rather than in the constructor, and once.
//
// A newly built window cannot say how wide it needs to be: the readings are
// still dashes, the state lines are still empty, and the platform style and its
// font have not been applied yet, so every size hint is far too small. By the
// time the window is first shown it has been polled at, so the labels carry the
// text that actually decides the width.
void TrayWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_sized)
        return;
    m_sized = true;

    // The largest tab, so switching tabs does not resize the window and no tab
    // opens clipped.
    QSize wanted;
    int narrowest = 0;
    for (const QWidget *page : std::as_const(m_ui->pages)) {
        wanted = wanted.expandedTo(page->sizeHint());
        narrowest = std::max(narrowest, page->minimumSizeHint().width());
    }
    // Below this the pages clip instead of scrolling sideways, which is the
    // right trade for a console — a sideways scrollbar under a row of sliders
    // is worse than a window that refuses to get narrower.
    setMinimumWidth(narrowest + kSideChromePx);
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        resize(std::min(wanted.width() + kSideChromePx, available.width()),
               std::min(wanted.height() + kVerticalChromePx, available.height() * 9 / 10));
    }
}

TrayWindow::~TrayWindow()
{
    delete m_ui;
}

void TrayWindow::build()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    // Outside the scroll area on purpose: whether the daemon is answering is
    // the first thing to know, and it must not be something to scroll for.
    m_ui->connection = new QLabel(this);
    m_ui->connection->setObjectName(QStringLiteral("connection"));
    m_ui->connection->setWordWrap(true);
    m_ui->connection->setContentsMargins(8, 6, 8, 6);
    outer->addWidget(m_ui->connection);

    m_ui->tabs = new QTabWidget(this);
    m_ui->tabs->setObjectName(QStringLiteral("tabs"));
    outer->addWidget(m_ui->tabs, 1);

    // One tab is a scrollable column of groups. The lambda returns the layout
    // to fill; the stretch at its end keeps the groups at the top of a tab
    // that has room to spare.
    const auto addPage = [this](const QString &title) {
        auto *area = new QScrollArea(m_ui->tabs);
        area->setWidgetResizable(true);
        area->setFrameShape(QFrame::NoFrame);
        area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *page = new QWidget(area);
        area->setWidget(page);
        m_ui->tabs->addTab(area, title);
        m_ui->pages.append(page);
        return new QVBoxLayout(page);
    };

    // What only reports, separately from what commands: readings and
    // indicators are what a glance is for, and they must not be behind the tab
    // somebody left the sliders on.
    auto *nowLayout = addPage(QStringLiteral("Now"));
    QWidget *const self = m_ui->pages.constLast();

    auto *readings = new QGroupBox(QStringLiteral("Readings"), self);
    auto *readingsForm = new QFormLayout(readings);
    m_ui->packageC = addReading(readingsForm, QStringLiteral("Package:"),
                                QStringLiteral("pkg_c"));
    m_ui->boardC = addReading(readingsForm, QStringLiteral("Board:"), QStringLiteral("board_c"));
    m_ui->nvmeC = addReading(readingsForm, QStringLiteral("NVMe:"), QStringLiteral("nvme_c"));
    m_ui->fanRpm = addReading(readingsForm, QStringLiteral("Fan:"), QStringLiteral("fan_rpm"));
    m_ui->packageW = addReading(readingsForm, QStringLiteral("Package power:"),
                                QStringLiteral("pkg_w"));
    m_ui->coreW = addReading(readingsForm, QStringLiteral("Core power:"),
                             QStringLiteral("core_w"));
    m_ui->uncoreW = addReading(readingsForm, QStringLiteral("Uncore power:"),
                               QStringLiteral("uncore_w"));
    m_ui->cpuMhz = addReading(readingsForm, QStringLiteral("CPU frequency:"),
                              QStringLiteral("cpu_mhz"));
    m_ui->gpuMhz = addReading(readingsForm, QStringLiteral("iGPU frequency:"),
                              QStringLiteral("gpu_mhz"));
    m_ui->load1 = addReading(readingsForm, QStringLiteral("Load average:"),
                             QStringLiteral("load1"));
    nowLayout->addWidget(readings);

    auto *state = new QGroupBox(QStringLiteral("State"), self);
    auto *stateLayout = new QVBoxLayout(state);
    m_ui->applied = new QLabel(QStringLiteral("—"), state);
    m_ui->applied->setObjectName(QStringLiteral("applied"));
    m_ui->applied->setWordWrap(true);
    stateLayout->addWidget(m_ui->applied);
    m_ui->powerSource = new QLabel(QStringLiteral("—"), state);
    m_ui->powerSource->setObjectName(QStringLiteral("power_source"));
    stateLayout->addWidget(m_ui->powerSource);
    m_ui->live = addIndicator(stateLayout,
                              QStringLiteral("Live tuning is in force and is never saved."),
                              QStringLiteral("live_tuning"));
    // The guard is a safety interlock, so its state has to be visible: while it
    // clamps, the sliders below say what was asked for and not what is applied.
    m_ui->guard = addIndicator(stateLayout,
                               QStringLiteral("Emergency thermal guard engaged: the package is "
                                              "near the hardware cutout and power is clamped."),
                               QStringLiteral("guard"));
    m_ui->guard->setWordWrap(true);
    // Something else is writing the same sysfs nodes, which this daemon is
    // supposed to own alone.
    m_ui->mismatch = addIndicator(stateLayout,
                                  QStringLiteral("A control read back a value the daemon did not "
                                                 "write: another process is writing them too."),
                                  QStringLiteral("mismatch"));
    m_ui->mismatch->setWordWrap(true);
    m_ui->sensors = addIndicator(stateLayout, QStringLiteral("Some sensors could not be read."),
                                 QStringLiteral("sensors"));
    nowLayout->addWidget(state);
    nowLayout->addStretch(1);

    // The three deliberate acts, together: choosing an envelope, demanding
    // maximum airflow, and recording what the result sounds like.
    auto *modeLayout = addPage(QStringLiteral("Mode"));
    QWidget *const modePage = m_ui->pages.constLast();

    auto *modeBox = new QGroupBox(QStringLiteral("Mode"), modePage);
    auto *modeForm = new QFormLayout(modeBox);
    m_ui->mode = new QComboBox(modeBox);
    m_ui->mode->setObjectName(QStringLiteral("mode"));
    modeForm->addRow(QStringLiteral("Active mode:"), m_ui->mode);
    modeLayout->addWidget(modeBox);

    // Its own tab, deliberately not beside the mode: following the ambient
    // light sensor is a separate function, and selecting a thermal mode never
    // moves the backlight. Switching this off is one click, takes effect at
    // once, and is remembered across a reboot; neither switching it on nor off
    // moves the backlight by itself.
    auto *brightnessLayout = addPage(QStringLiteral("Backlight"));
    QWidget *const brightnessPage = m_ui->pages.constLast();

    auto *brightness = new QGroupBox(QStringLiteral("Backlight"), brightnessPage);
    auto *brightnessForm = new QFormLayout(brightness);
    m_ui->autoBrightness =
            new QCheckBox(QStringLiteral("Follow the ambient light sensor"), brightness);
    m_ui->autoBrightness->setObjectName(QStringLiteral("auto_brightness"));
    m_ui->autoBrightness->setToolTip(QStringLiteral("Works in a window around the level you set. "
                                                    "Your own adjustments move that window and "
                                                    "are never overridden."));
    brightnessForm->addRow(QString(), m_ui->autoBrightness);
    m_ui->brightnessProfile = new QComboBox(brightness);
    m_ui->brightnessProfile->setObjectName(QStringLiteral("brightness_profile"));
    brightnessForm->addRow(QStringLiteral("Profile:"), m_ui->brightnessProfile);
    m_ui->brightnessState = new QLabel(QStringLiteral("—"), brightness);
    m_ui->brightnessState->setObjectName(QStringLiteral("brightness_state"));
    m_ui->brightnessState->setWordWrap(true);
    brightnessForm->addRow(QStringLiteral("Now:"), m_ui->brightnessState);

    // How far the automation may wander from the level you set, and the two
    // absolute bounds. These three are the answer to "the range is too narrow",
    // and they are judged by eye in the room they are meant for, which is why
    // they are sliders here and not only numbers in the file. Like the thermal
    // sliders they apply at once and are never written back; the curve is not
    // here on purpose — its shape is calibrated from recorded telemetry.
    addTuner(brightnessForm, QStringLiteral("Range around your level:"),
             QStringLiteral("brightness_span"), 0, 100, 1, &m_ui->brightnessSpan,
             &m_ui->brightnessSpanValue);
    addTuner(brightnessForm, QStringLiteral("Never below:"), QStringLiteral("brightness_floor"), 0,
             100, 1, &m_ui->brightnessFloor, &m_ui->brightnessFloorValue);
    addTuner(brightnessForm, QStringLiteral("Never above:"),
             QStringLiteral("brightness_ceiling"), 0, 100, 1, &m_ui->brightnessCeiling,
             &m_ui->brightnessCeilingValue);
    auto *brightnessReset = new QPushButton(QStringLiteral("Back to the profile"), brightness);
    brightnessReset->setObjectName(QStringLiteral("brightness_reset"));
    brightnessReset->setToolTip(QStringLiteral("Drops the live values and returns to the three the "
                                              "configuration file defines for this profile."));
    brightnessForm->addRow(QString(), brightnessReset);
    brightnessLayout->addWidget(brightness);
    brightnessLayout->addStretch(1);

    auto *tuningLayout = addPage(QStringLiteral("Tuning"));
    QWidget *const tuningPage = m_ui->pages.constLast();

    auto *tuning = new QGroupBox(QStringLiteral("Live tuning"), tuningPage);
    auto *tuningForm = new QFormLayout(tuning);

    addTuner(tuningForm, QStringLiteral("PL1, long term:"), QStringLiteral("pl1"), 1,
             kMaxConfirmedPl1W, 1, &m_ui->pl1, &m_ui->pl1Value);
    addTuner(tuningForm, QStringLiteral("PL2, short term:"), QStringLiteral("pl2"), 1,
             kMaxConfirmedPl2W, 1, &m_ui->pl2, &m_ui->pl2Value);
    addTuner(tuningForm, QStringLiteral("Frequency ceiling:"), QStringLiteral("max_perf_pct"),
             kMinPerfPct, 100, 1, &m_ui->maxPerfPct, &m_ui->maxPerfPctValue);
    addTuner(tuningForm, QStringLiteral("iGPU ceiling:"), QStringLiteral("gt_max_mhz"), kMinGtMhz,
             kMaxGtMhz, 50, &m_ui->gtMaxMhz, &m_ui->gtMaxMhzValue);

    m_ui->epp = new QComboBox(tuning);
    m_ui->epp->setObjectName(QStringLiteral("epp"));
    // Epp::Default is not offered: see the comment on it in sample.h.
    for (const Epp value : { Epp::Performance, Epp::BalancePerformance, Epp::BalancePower,
                             Epp::Power }) {
        m_ui->epp->addItem(eppToString(value), static_cast<int>(value));
    }
    tuningForm->addRow(QStringLiteral("Energy preference:"), m_ui->epp);

    m_ui->profile = new QComboBox(tuning);
    m_ui->profile->setObjectName(QStringLiteral("profile"));
    for (const Profile value : { Profile::Quiet, Profile::Balanced, Profile::Performance })
        m_ui->profile->addItem(profileToString(value), static_cast<int>(value));
    // The platform profile is the EC fan curve, which is the noise-versus-
    // temperature dial and the only way to move the curve as a whole.
    tuningForm->addRow(QStringLiteral("Fan curve:"), m_ui->profile);

    m_ui->turbo = new QCheckBox(QStringLiteral("Turbo"), tuning);
    m_ui->turbo->setObjectName(QStringLiteral("turbo"));
    tuningForm->addRow(QString(), m_ui->turbo);
    m_ui->hwpBoost = new QCheckBox(QStringLiteral("HWP dynamic boost"), tuning);
    m_ui->hwpBoost->setObjectName(QStringLiteral("hwp_boost"));
    tuningForm->addRow(QString(), m_ui->hwpBoost);

    auto *resetButton = new QPushButton(QStringLiteral("Drop live tuning"), tuning);
    resetButton->setObjectName(QStringLiteral("reset"));
    auto *snippetButton = new QPushButton(QStringLiteral("Copy as configuration snippet"), tuning);
    snippetButton->setObjectName(QStringLiteral("copy_snippet"));
    snippetButton->setToolTip(QStringLiteral("Puts these values on the clipboard as a mode "
                                             "section. Promoting them into the configuration "
                                             "file stays a deliberate edit."));
    auto *tuningButtons = new QHBoxLayout;
    tuningButtons->addWidget(resetButton);
    tuningButtons->addWidget(snippetButton);
    tuningForm->addRow(tuningButtons);
    tuningLayout->addWidget(tuning);
    tuningLayout->addStretch(1);

    auto *fan = new QGroupBox(QStringLiteral("Fan"), modePage);
    auto *fanLayout = new QHBoxLayout(fan);
    m_ui->fanState = new QLabel(QStringLiteral("—"), fan);
    m_ui->fanState->setObjectName(QStringLiteral("fan_state"));
    fanLayout->addWidget(m_ui->fanState);
    // Full speed is roughly 6100 RPM, far past what is tolerable. It is
    // reachable only on explicit demand and never by automatic control, so it
    // is a command rather than a state the window keeps.
    auto *fanMaxButton = new QPushButton(QStringLiteral("Maximum, about 6100 RPM"), fan);
    fanMaxButton->setObjectName(QStringLiteral("fan_max"));
    fanMaxButton->setToolTip(QStringLiteral("Independent of the mode. Automatic control never "
                                            "goes above roughly 4500 RPM."));
    auto *fanAutoButton = new QPushButton(QStringLiteral("Automatic"), fan);
    fanAutoButton->setObjectName(QStringLiteral("fan_auto"));
    fanLayout->addWidget(fanMaxButton);
    fanLayout->addWidget(fanAutoButton);
    modeLayout->addWidget(fan);

    auto *verdict = new QGroupBox(QStringLiteral("Verdict"), modePage);
    auto *verdictLayout = new QHBoxLayout(verdict);
    // A judgement made by ear that cannot be traced back to the numbers that
    // produced it is worthless, so it is stamped onto the current sample.
    const auto addMark = [verdict, verdictLayout](const QString &caption, const QString &name) {
        auto *button = new QPushButton(caption, verdict);
        button->setObjectName(name);
        verdictLayout->addWidget(button);
        return button;
    };
    auto *goodButton = addMark(QStringLiteral("Good"), QStringLiteral("mark_good"));
    auto *loudButton = addMark(QStringLiteral("Too loud"), QStringLiteral("mark_too_loud"));
    auto *slowButton = addMark(QStringLiteral("Too slow"), QStringLiteral("mark_too_slow"));
    modeLayout->addWidget(verdict);
    modeLayout->addStretch(1);

    // The console's own tab: everything else in this window is about the
    // machine, and the one thing that is about the console itself does not
    // belong beside it.
    auto *trayLayout = addPage(QStringLiteral("Tray"));
    QWidget *const trayPage = m_ui->pages.constLast();

    auto *trayBox = new QGroupBox(QStringLiteral("Tray"), trayPage);
    auto *trayForm = new QVBoxLayout(trayBox);
    m_ui->autostart = new QCheckBox(QStringLiteral("Start with the session"), trayBox);
    m_ui->autostart->setObjectName(QStringLiteral("autostart"));
    m_ui->autostart->setChecked(autostart::enabled());
    trayForm->addWidget(m_ui->autostart);
    // The units are made when the session starts, so the entry is read then and
    // not now. A switch that appears to do nothing is worse than no switch.
    auto *autostartNote = new QLabel(
            QStringLiteral("Takes effect from the next login. Switched off, it stays off: the "
                           "state is the autostart entry itself, and there is nothing else to "
                           "bring it back."),
            trayBox);
    autostartNote->setObjectName(QStringLiteral("autostart_note"));
    autostartNote->setWordWrap(true);
    trayForm->addWidget(autostartNote);
    trayLayout->addWidget(trayBox);
    trayLayout->addStretch(1);

    // Outside the tabs, like the banner: a command sent from one tab is
    // answered where the person is looking, not where the control was.
    m_ui->notice = new QLabel(this);
    m_ui->notice->setObjectName(QStringLiteral("notice"));
    m_ui->notice->setWordWrap(true);
    m_ui->notice->setContentsMargins(8, 0, 8, 6);
    outer->addWidget(m_ui->notice);

    // Everything that sends a command, as opposed to everything that reports
    // one. Grey while nothing is answering: a slider parked at its minimum is
    // otherwise indistinguishable from a daemon holding 1 W.
    m_ui->commands = { modeBox, brightness, tuning, fan, verdict };

    for (QSlider *slider : { m_ui->pl1, m_ui->pl2, m_ui->maxPerfPct, m_ui->gtMaxMhz }) {
        connect(slider, &QSlider::valueChanged, this, [this]() {
            updateTunerLabels();
            sendCurrentSetpoints();
        });
    }
    for (QComboBox *box : { m_ui->epp, m_ui->profile })
        connect(box, &QComboBox::currentIndexChanged, this, &TrayWindow::sendCurrentSetpoints);
    for (QCheckBox *box : { m_ui->turbo, m_ui->hwpBoost })
        connect(box, &QCheckBox::toggled, this, &TrayWindow::sendCurrentSetpoints);

    connect(m_ui->mode, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_updating || index < 0)
            return;
        const QString id = m_ui->mode->itemData(index).toString();
        emit modeSelected(id);
        sendCommand(QStringLiteral("mode ") + id);
    });
    connect(m_ui->autoBrightness, &QCheckBox::toggled, this, [this](bool on) {
        if (m_updating)
            return;
        sendCommand(on ? QStringLiteral("brightness on") : QStringLiteral("brightness off"));
    });
    connect(m_ui->brightnessProfile, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_updating || index < 0)
            return;
        sendCommand(QStringLiteral("brightness profile ")
                    + m_ui->brightnessProfile->itemData(index).toString());
    });
    connect(m_ui->brightnessSpan, &QSlider::valueChanged, this, [this]() {
        updateTunerLabels();
        sendBrightnessWindow();
    });
    // The pair cannot cross: the daemon refuses a floor above a ceiling, so the
    // slider being dragged pushes the other one ahead of it rather than
    // reaching a value that is always refused.
    connect(m_ui->brightnessFloor, &QSlider::valueChanged, this, [this](int value) {
        if (value > m_ui->brightnessCeiling->value()) {
            m_ui->brightnessCeiling->setValue(value);
            return;
        }
        updateTunerLabels();
        sendBrightnessWindow();
    });
    connect(m_ui->brightnessCeiling, &QSlider::valueChanged, this, [this](int value) {
        if (value < m_ui->brightnessFloor->value()) {
            m_ui->brightnessFloor->setValue(value);
            return;
        }
        updateTunerLabels();
        sendBrightnessWindow();
    });
    connect(brightnessReset, &QPushButton::clicked, this,
            [this]() { sendCommand(QStringLiteral("brightness reset")); });
    connect(m_ui->autostart, &QCheckBox::toggled, this, &TrayWindow::setAutostart);
    connect(fanMaxButton, &QPushButton::clicked, this,
            [this]() { sendCommand(QStringLiteral("fan max")); });
    connect(fanAutoButton, &QPushButton::clicked, this,
            [this]() { sendCommand(QStringLiteral("fan auto")); });
    connect(resetButton, &QPushButton::clicked, this,
            [this]() { sendCommand(QStringLiteral("reset")); });
    connect(snippetButton, &QPushButton::clicked, this, &TrayWindow::copyConfigSnippet);
    connect(goodButton, &QPushButton::clicked, this, &TrayWindow::markGood);
    connect(loudButton, &QPushButton::clicked, this, &TrayWindow::markTooLoud);
    connect(slowButton, &QPushButton::clicked, this, &TrayWindow::markTooSlow);

    updateTunerLabels();
}

void TrayWindow::applyStatus(const TrayClient::Status &status)
{
    m_updating = true;
    // A status that parsed is the daemon answering, whatever the connection
    // signal has had a chance to say yet.
    if (status.valid)
        setCommandsEnabled(true);

    m_ui->packageC->setText(formatted(status.packageC, QStringLiteral("°C"), 1));
    m_ui->boardC->setText(formatted(status.boardC, QStringLiteral("°C"), 1));
    m_ui->nvmeC->setText(formatted(status.nvmeC, QStringLiteral("°C"), 1));
    m_ui->fanRpm->setText(formatted(status.fanRpm, QStringLiteral("RPM"), 0));
    m_ui->packageW->setText(formatted(status.packageW, QStringLiteral("W"), 2));
    m_ui->coreW->setText(formatted(status.coreW, QStringLiteral("W"), 2));
    m_ui->uncoreW->setText(formatted(status.uncoreW, QStringLiteral("W"), 2));
    m_ui->cpuMhz->setText(formatted(status.cpuMhz, QStringLiteral("MHz"), 0));
    m_ui->gpuMhz->setText(formatted(status.gpuMhz, QStringLiteral("MHz"), 0));
    m_ui->load1->setText(formatted(status.load1, QString(), 2));

    if (m_modeIds != status.modeIds) {
        m_modeIds = status.modeIds;
        m_ui->mode->clear();
        for (int i = 0; i < status.modeIds.size(); ++i) {
            m_ui->mode->addItem(status.modeLabels.value(i, status.modeIds.at(i)),
                                status.modeIds.at(i));
        }
    }
    const int modeIndex = status.modeIds.indexOf(status.modeId);
    if (modeIndex >= 0)
        m_ui->mode->setCurrentIndex(modeIndex);

    // The sliders show what was asked for, not what reached the hardware: when
    // the guard clamps, the difference between the two is the information.
    m_ui->pl1->setValue(status.requested.pl1W);
    m_ui->pl2->setValue(status.requested.pl2W);
    m_ui->maxPerfPct->setValue(status.requested.maxPerfPct);
    m_ui->gtMaxMhz->setValue(status.requested.gtMaxMhz);
    m_ui->epp->setCurrentIndex(m_ui->epp->findData(static_cast<int>(status.requested.epp)));
    m_ui->profile->setCurrentIndex(
            m_ui->profile->findData(static_cast<int>(status.requested.profile)));
    m_ui->turbo->setChecked(!status.requested.noTurbo);
    m_ui->hwpBoost->setChecked(status.requested.hwpBoost);
    updateTunerLabels();

    // The applied fan mode, because the guard forces full speed by itself and
    // the window has to show the fan that is actually running.
    m_ui->fanState->setText(status.applied.fanMode == 0
                                    ? QStringLiteral("Fan: full speed")
                                    : QStringLiteral("Fan: automatic, EC controlled"));

    m_ui->applied->setText(
            QStringLiteral("Applied: PL1 %1 W, PL2 %2 W, ceiling %3 %, turbo %4, EPP %5, "
                           "fan curve %6, iGPU %7 MHz")
                    .arg(status.applied.pl1W)
                    .arg(status.applied.pl2W)
                    .arg(status.applied.maxPerfPct)
                    .arg(status.applied.noTurbo ? QStringLiteral("off") : QStringLiteral("on"),
                         eppToString(status.applied.epp),
                         profileToString(status.applied.profile))
                    .arg(status.applied.gtMaxMhz));
    m_ui->powerSource->setText(status.onAc ? QStringLiteral("On mains power")
                                           : QStringLiteral("On battery"));
    if (m_brightnessProfileIds != status.brightnessProfileIds) {
        m_brightnessProfileIds = status.brightnessProfileIds;
        m_ui->brightnessProfile->clear();
        for (int i = 0; i < status.brightnessProfileIds.size(); ++i) {
            m_ui->brightnessProfile->addItem(
                    status.brightnessProfileLabels.value(i, status.brightnessProfileIds.at(i)),
                    status.brightnessProfileIds.at(i));
        }
    }
    const int brightnessIndex = status.brightnessProfileIds.indexOf(status.brightnessProfileId);
    if (brightnessIndex >= 0)
        m_ui->brightnessProfile->setCurrentIndex(brightnessIndex);
    m_ui->autoBrightness->setChecked(status.autoBrightness);
    m_ui->autoBrightness->setEnabled(status.brightnessAvailable);
    m_ui->brightnessProfile->setEnabled(status.brightnessAvailable);
    m_ui->brightnessSpan->setValue(status.brightnessSpanPct);
    m_ui->brightnessFloor->setValue(status.brightnessFloorPct);
    m_ui->brightnessCeiling->setValue(status.brightnessCeilingPct);
    for (QSlider *slider :
         { m_ui->brightnessSpan, m_ui->brightnessFloor, m_ui->brightnessCeiling })
        slider->setEnabled(status.brightnessAvailable);
    updateTunerLabels();
    m_ui->brightnessState->setText(
            status.brightnessAvailable
                    ? QStringLiteral("%1, your level %2, ambient %3")
                              .arg(formatted(status.brightness, QStringLiteral("%"), 0),
                                   formatted(status.brightnessBase, QStringLiteral("%"), 0),
                                   formatted(status.light, QString(), 0))
                    : QStringLiteral("No backlight control on this machine."));

    m_ui->live->setVisible(status.liveTuning);
    m_ui->guard->setVisible(status.guardEngaged);
    m_ui->mismatch->setVisible(status.writeMismatch);
    m_ui->sensors->setVisible(status.sensorsDegraded);

    m_updating = false;
}

void TrayWindow::sendBrightnessWindow()
{
    if (m_updating)
        return;
    sendCommand(QStringLiteral("brightness set span_pct=%1 floor_pct=%2 ceiling_pct=%3")
                        .arg(m_ui->brightnessSpan->value())
                        .arg(m_ui->brightnessFloor->value())
                        .arg(m_ui->brightnessCeiling->value()));
}

void TrayWindow::sendCurrentSetpoints()
{
    if (m_updating)
        return;
    const auto onOff = [](bool value) {
        return value ? QStringLiteral("on") : QStringLiteral("off");
    };
    // Both selectors carry their enum as item data, so the command is built
    // from the value rather than from whatever the item happens to be labelled.
    const QString epp = eppToString(static_cast<Epp>(m_ui->epp->currentData().toInt()));
    const QString profile =
            profileToString(static_cast<Profile>(m_ui->profile->currentData().toInt()));
    sendCommand(QStringLiteral("set pl1=%1 pl2=%2 max_perf_pct=%3 gt_max_mhz=%4 turbo=%5 "
                               "hwp_boost=%6 epp=%7 profile=%8")
                        .arg(m_ui->pl1->value())
                        .arg(m_ui->pl2->value())
                        .arg(m_ui->maxPerfPct->value())
                        .arg(m_ui->gtMaxMhz->value())
                        .arg(onOff(m_ui->turbo->isChecked()), onOff(m_ui->hwpBoost->isChecked()),
                             epp, profile));
}

void TrayWindow::setAutostart(bool on)
{
    if (m_updating)
        return;
    QString error;
    if (autostart::setEnabled(on, &error)) {
        m_ui->notice->setText(
                on ? QStringLiteral("Will start with the session from the next login.")
                   : QStringLiteral("Will not start with the session."));
        return;
    }
    // The switch goes back, because the file is the state and the file did not
    // change. A checkbox that stayed on after a failed write would be a lie.
    m_updating = true;
    m_ui->autostart->setChecked(!on);
    m_updating = false;
    m_ui->notice->setText(QStringLiteral("autostart → %1").arg(error));
}

void TrayWindow::copyConfigSnippet()
{
    // The daemon renders the snippet, so the text is exactly what the
    // configuration parser would read back.
    const QString reply = m_client.send(QStringLiteral("snippet"));
    if (!reply.startsWith(QLatin1String("ok "))) {
        m_ui->notice->setText(QStringLiteral("snippet → %1")
                                      .arg(reply.isEmpty() ? QStringLiteral("no reply") : reply));
        return;
    }
    QGuiApplication::clipboard()->setText(
            QString::fromUtf8(QByteArray::fromBase64(reply.mid(3).toUtf8())));
    m_ui->notice->setText(QStringLiteral("Configuration snippet copied to the clipboard."));
}

void TrayWindow::markGood()
{
    sendCommand(QStringLiteral("mark good"));
}

void TrayWindow::markTooLoud()
{
    sendCommand(QStringLiteral("mark too-loud"));
}

void TrayWindow::markTooSlow()
{
    sendCommand(QStringLiteral("mark too-slow"));
}

void TrayWindow::onConnectionChanged(bool connected)
{
    if (connected) {
        m_ui->connection->setText(QStringLiteral("Connected to the daemon on %1")
                                          .arg(m_client.socketPath()));
    } else {
        // Naming the socket and the two things that usually explain its
        // silence, because "not answering" on its own leaves the person
        // guessing at which of them it is.
        m_ui->connection->setText(
                QStringLiteral("The daemon is not answering on %1.\n"
                               "Is ombrad running, and are you in the %2 group? "
                               "A group added while you were logged in only takes effect "
                               "after a fresh login.")
                        .arg(m_client.socketPath(),
                             QString::fromLatin1(kControlSocketGroup)));
    }
    setCommandsEnabled(connected);
}

void TrayWindow::setCommandsEnabled(bool enabled)
{
    for (QWidget *widget : m_ui->commands)
        widget->setEnabled(enabled);
}

void TrayWindow::sendCommand(const QString &command)
{
    const QString reply = m_client.sendAndRefresh(command);
    m_ui->notice->setText(QStringLiteral("%1 → %2")
                                  .arg(command, reply.isEmpty() ? QStringLiteral("no reply")
                                                                : reply));
}

void TrayWindow::updateTunerLabels()
{
    m_ui->pl1Value->setText(QStringLiteral("%1 W").arg(m_ui->pl1->value()));
    m_ui->pl2Value->setText(QStringLiteral("%1 W").arg(m_ui->pl2->value()));
    m_ui->maxPerfPctValue->setText(QStringLiteral("%1 %").arg(m_ui->maxPerfPct->value()));
    m_ui->gtMaxMhzValue->setText(QStringLiteral("%1 MHz").arg(m_ui->gtMaxMhz->value()));
    // Plus and minus, because the span is symmetric around the level the
    // person set and reading it as an absolute level would be wrong.
    m_ui->brightnessSpanValue->setText(
            QStringLiteral("± %1 pp").arg(m_ui->brightnessSpan->value()));
    m_ui->brightnessFloorValue->setText(QStringLiteral("%1 %").arg(m_ui->brightnessFloor->value()));
    m_ui->brightnessCeilingValue->setText(
            QStringLiteral("%1 %").arg(m_ui->brightnessCeiling->value()));
}
