#pragma once

#include "reading/sleep_screen.h"

#include <QImage>
#include <QString>
#include <QRect>

namespace kobalt::reader {
enum class SleepStatus { Sleeping, Charging, Charged, PoweredOff };
QString sleepStatusText(SleepStatus status);
// Rounds up to whole minutes. Estimates above 120 minutes use hours and minutes.
// A negative estimate is unavailable and has no label.
QString sleepTimeLeft(qint64 milliseconds);
// Fits the cover without cropping. Unused space is always black.
QImage sleepCoverBackground(const QImage &cover, QSize size);
// The area a tile covers. It does not depend on the status, so a battery
// update can repaint and refresh only this area.
QRect sleepScreenCardRect(QSize size, bool showRemaining, sleep::Tile tile = sleep::Tile::Card);
// Takes ownership of one background and paints in place. The caller
// releases the completed frame to the host before the device suspends.
// percent is the reading progress for tiles that draw it; -1 leaves it out.
QImage sleepScreenImage(QImage page, const QString &progress, const QString &remaining, bool dark,
                        int batteryPercent = -1, SleepStatus status = SleepStatus::Sleeping, bool dimPage = true,
                        sleep::Tile tile = sleep::Tile::Card, int percent = -1);
}
