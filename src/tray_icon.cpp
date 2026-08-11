// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "tray_icon.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStringList>

#include <cmath>

namespace {

// The name the icon is installed under, in `share/icons/hicolor/scalable/apps`, and
// the name the desktop entry gives. A theme lookup takes the name without the
// extension; a fallback directory is searched for the file itself.
constexpr const char *kIconName = "sombra";
// Puts a directory of pictures ahead of the icon theme, for a run from a build tree.
constexpr const char *kIconsOverride = "SOMBRA_ICONS_DIR";

// Drawn on the 256-unit grid of `desktop/sombra.svg`, so the panel and the
// launcher show one picture rather than two that resemble each other.
constexpr qreal kGrid = 256;
constexpr qreal kPlateRadius = 56;

// The sun sits above and right of the middle. That is the room the cloud needs to
// come in from below and still stay on the plate.
constexpr qreal kSunX = 136;
constexpr qreal kSunY = 110;
constexpr qreal kSunRadius = 54;
constexpr qreal kRayInner = 64;
constexpr qreal kRayOuter = 88;
constexpr qreal kRayWidth = 12;
constexpr int kRayCount = 8;
// Half a step of rotation, which puts two rays near the top and two near the
// bottom instead of one of each pointing straight up and down.
constexpr qreal kRayTurn = M_PI / kRayCount;

// The cloud: three bumps over a flat base, filled as one outline so the overlaps
// leave no seam. There is one shape for every state and only its seat changes,
// because switching a mode has to move the shade rather than redraw the weather.
struct Bump {
    qreal x;
    qreal y;
    qreal radius;
};
constexpr Bump kBumps[] = { { 78, 164, 44 }, { 126, 150, 38 }, { 168, 168, 30 } };
constexpr qreal kBaseX = 28;
constexpr qreal kBaseY = 168;
constexpr qreal kBaseWidth = 156;
constexpr qreal kBaseHeight = 46;
constexpr qreal kBaseRadius = 23;

// The cloud is held off the sun by a band of the plate's own colour, in grid
// units because it is a distance on the drawing. Without it, at 22 pixels the
// shared edge between a light cloud and a warm sun is a single blended pixel and
// the two stop being two objects. Measured on the drawing: below about seven
// units the band is thinner than half a pixel at that size and vanishes into the
// antialiasing, above about fourteen it reads as an outline drawn round a sticker.
constexpr qreal kGap = 11;

// Measured against a light panel (#f6f5f4) and a dark one (#202225): the plate
// clears 3.5:1 both ways in either colour, which no single warm tone does. The
// sun and the cloud are then seen only against the plate.
//
// Flat, where the launcher icon's plate is a gradient. The dark end of that
// gradient measures 1.3:1 against a dark panel, and a plate with a corner that
// dissolves into the bar reads as a drawing fault rather than as a shape.
const QColor kPlate(47, 127, 209);       // blue: nothing is clamping
const QColor kPlateGuard(214, 74, 58);   // red: the emergency guard is clamping
const QColor kSun(255, 214, 102);        // warm: the mode is known
const QColor kSunAbsent(170, 177, 186);  // grey: nothing is known about the mode
const QColor kCloud(244, 247, 251);
const QColor kCloudAbsent(223, 227, 232);

// The sizes a panel asks for, drawn at each rather than scaled from one. A gap of
// one pixel does not survive a smooth reduction from 256 pixels to 22.
constexpr int kSizes[] = { 16, 22, 24, 32, 48, 64 };

// Where the cloud sits, which is the whole of what the picture says about the
// mode.
enum class Cloud {
    Middle, // the coolest envelope: the cloud alone, and no sun at all
    OnSun,  // in between: the cloud over the lower half of the sun
    None,   // the widest envelope: the sun on its own
};

QPainterPath cloudPath(qreal grow)
{
    QPainterPath path;
    for (const Bump &bump : kBumps) {
        QPainterPath bumpPath;
        bumpPath.addEllipse(QPointF(bump.x, bump.y), bump.radius + grow, bump.radius + grow);
        path = path.united(bumpPath);
    }
    QPainterPath base;
    base.addRoundedRect(QRectF(kBaseX - grow, kBaseY - grow, kBaseWidth + 2 * grow,
                              kBaseHeight + 2 * grow),
                        kBaseRadius + grow, kBaseRadius + grow);
    return path.united(base);
}

// What moves the cloud from its seat on the sun to the middle of the plate.
// Derived from the shape rather than written down, so the two cannot drift apart.
QPointF middleOffset()
{
    const QPointF seat = cloudPath(0).boundingRect().center();
    return QPointF(kGrid / 2 - seat.x(), kGrid / 2 - seat.y());
}

// The mode's position in the table decides the seat. The ends of the table are the
// ends of the scale, and everything in between is the one picture that says "some
// way in": there are three modes, so in practice each has a seat of its own.
Cloud seatFor(int modeIndex, int modeCount)
{
    if (modeIndex >= modeCount - 1)
        return Cloud::None;
    if (modeIndex <= 0)
        return Cloud::Middle;
    return Cloud::OnSun;
}

void paint(QPainter &painter, int size, const QColor &plate, const QColor &sun, const QColor &cloud,
           Cloud seat)
{
    const qreal s = size / kGrid;
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(s, s);
    painter.setPen(Qt::NoPen);
    painter.setBrush(plate);
    painter.drawRoundedRect(QRectF(0, 0, kGrid, kGrid), kPlateRadius, kPlateRadius);

    // With the cloud in the middle there is no sun to be seen behind it, so none
    // is drawn: the rays would come out around the cloud and the coolest envelope
    // would look like a mode with the light still on.
    if (seat != Cloud::Middle) {
        const QPointF centre(kSunX, kSunY);
        painter.setPen(QPen(sun, kRayWidth, Qt::SolidLine, Qt::RoundCap));
        for (int i = 0; i < kRayCount; ++i) {
            const qreal angle = kRayTurn + i * 2 * M_PI / kRayCount;
            const QPointF direction(std::cos(angle), std::sin(angle));
            painter.drawLine(centre + direction * kRayInner, centre + direction * kRayOuter);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(sun);
        painter.drawEllipse(centre, kSunRadius, kSunRadius);
    }

    if (seat == Cloud::None)
        return;

    if (seat == Cloud::Middle) {
        const QPointF offset = middleOffset();
        painter.translate(offset);
        painter.setBrush(cloud);
        painter.drawPath(cloudPath(0));
        return;
    }

    // The band first, in the plate's colour, then the cloud inside it. It is only
    // ever drawn where there is a sun to be held off.
    painter.setBrush(plate);
    painter.drawPath(cloudPath(kGap));
    painter.setBrush(cloud);
    painter.drawPath(cloudPath(0));
}

} // namespace

QIcon trayIcon(bool connected, bool guardEngaged, int modeIndex, int modeCount)
{
    // Nothing is known about the mode when the daemon is silent, and nothing is
    // known about it when the reply carries no mode either. One picture for both:
    // the icon speaks about the mode, and there is no mode to speak about. Which
    // of the two it is belongs to the tooltip and to the console's status line,
    // where there are words for it.
    const bool modeKnown = connected && modeCount > 0 && modeIndex >= 0;

    const QColor plate = guardEngaged ? kPlateGuard : kPlate;
    const QColor sun = modeKnown ? kSun : kSunAbsent;
    const QColor cloud = modeKnown ? kCloud : kCloudAbsent;
    const Cloud seat = modeKnown ? seatFor(modeIndex, modeCount) : Cloud::OnSun;

    QIcon icon;
    for (const int size : kSizes) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(painter, size, plate, sun, cloud, seat);
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

QIcon applicationIcon()
{
    // Prepended rather than appended: with the package installed the theme carries a
    // picture under this name already, and a build tree is asking about its own.
    if (const QByteArray override = qgetenv(kIconsOverride); !override.isEmpty()) {
        const QString directory = QString::fromLocal8Bit(override);
        QStringList paths = QIcon::fallbackSearchPaths();
        if (!paths.contains(directory))
            QIcon::setFallbackSearchPaths(QStringList { directory } + paths);
    }
    return QIcon::fromTheme(QString::fromLatin1(kIconName));
}
