// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "tray_icon.h"

#include <QPainter>
#include <QPixmap>

#include <algorithm>
#include <cmath>

namespace {

// Drawn on the 256-unit grid of `desktop/sombra.svg`, so the panel and the
// launcher show one picture rather than two that resemble each other.
constexpr qreal kGrid = 256;
constexpr qreal kPlateRadius = 56;
constexpr qreal kCentre = 128;
constexpr qreal kSunRadius = 66;
constexpr qreal kRayInner = 75;
constexpr qreal kRayOuter = 99;
constexpr qreal kRayWidth = 12;
constexpr int kRayCount = 8;
// Half a step of rotation, which puts two rays near the top and two near the
// bottom and leaves the persiana room to cross the sun at its full width.
constexpr qreal kRayTurn = M_PI / kRayCount;

// Coarser than the launcher icon's five slats on purpose. At 22 pixels a pitch
// of five is under two pixels and the shut end of the range merges into one dark
// block, which is the mode ceasing to be legible at the only size that matters.
constexpr int kSlatCount = 3;
// The persiana is square and centred: it spans the sun and its rays in both
// directions, so it closes over the whole of the light.
constexpr qreal kSlatOrigin = 32;
constexpr qreal kSlatSpan = 192;
// The fraction of a slat's pitch that is open, from the coolest envelope to the
// widest. The open end is the launcher icon's own proportion; the shut end keeps
// a gap, because a persiana closed to nothing is a rectangle.
constexpr qreal kOpenShut = 0.18;
constexpr qreal kOpenWide = 0.78;

// Measured against a light panel (#f6f5f4) and a dark one (#202225): the plate
// clears 3.5:1 both ways in either colour, which no single warm tone does. The
// sun and the slats are then seen only against the plate, at 2.5:1 and 10:1.
//
// Flat, where the launcher icon's plate is a gradient. The dark end of that
// gradient measures 1.3:1 against a dark panel, and a plate with a corner that
// dissolves into the bar reads as a drawing fault rather than as a shape.
const QColor kPlate(47, 127, 209);       // blue: nothing is clamping
const QColor kPlateGuard(214, 74, 58);   // red: the emergency guard is clamping
const QColor kSun(255, 214, 102);        // warm: the daemon is answering
const QColor kSunAbsent(176, 182, 190);  // grey: nothing is answering
const QColor kSlat(30, 34, 42);

// The sizes a panel asks for, drawn at each rather than scaled from one. Thin
// slats do not survive a smooth reduction from 256 pixels to 22.
constexpr int kSizes[] = { 16, 22, 24, 32, 48, 64 };

// How far the persiana is open. A mode that is not known yet gets no persiana at
// all rather than a guess at the middle.
qreal openFraction(int modeIndex, int modeCount)
{
    if (modeCount <= 0 || modeIndex < 0)
        return 0;
    // The ends of the table sit at the ends of the range, so the whole of it is
    // spent on telling the modes apart. Sharing the range with a position the
    // table never takes costs a pixel of slat at panel size, and a pixel is the
    // difference between two neighbouring modes.
    const int position = std::min(modeIndex, modeCount - 1);
    if (modeCount == 1)
        return kOpenWide;
    const qreal share = static_cast<qreal>(position) / static_cast<qreal>(modeCount - 1);
    return kOpenShut + (kOpenWide - kOpenShut) * share;
}

void paint(QPainter &painter, int size, const QColor &plate, const QColor &sun, qreal open)
{
    const qreal s = size / kGrid;
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(s, s);
    painter.setPen(Qt::NoPen);
    painter.setBrush(plate);
    painter.drawRoundedRect(QRectF(0, 0, kGrid, kGrid), kPlateRadius, kPlateRadius);

    const QPointF centre(kCentre, kCentre);
    painter.setPen(QPen(sun, kRayWidth, Qt::SolidLine, Qt::RoundCap));
    for (int i = 0; i < kRayCount; ++i) {
        const qreal angle = kRayTurn + i * 2 * M_PI / kRayCount;
        const QPointF direction(std::cos(angle), std::sin(angle));
        painter.drawLine(centre + direction * kRayInner, centre + direction * kRayOuter);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(sun);
    painter.drawEllipse(centre, kSunRadius, kSunRadius);

    if (open <= 0)
        return;

    painter.setBrush(kSlat);
    const qreal pitch = kSlatSpan / kSlatCount;
    const qreal height = pitch * (1.0 - open);
    for (int i = 0; i < kSlatCount; ++i) {
        const QRectF slat(kSlatOrigin, kSlatOrigin + i * pitch + (pitch - height) / 2, kSlatSpan,
                          height);
        painter.drawRoundedRect(slat, height / 2, height / 2);
    }
}

} // namespace

QIcon trayIcon(bool connected, bool guardEngaged, int modeIndex, int modeCount)
{
    const QColor plate = guardEngaged ? kPlateGuard : kPlate;
    const QColor sun = connected ? kSun : kSunAbsent;
    const qreal open = connected ? openFraction(modeIndex, modeCount) : 0;

    QIcon icon;
    for (const int size : kSizes) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(painter, size, plate, sun, open);
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}
