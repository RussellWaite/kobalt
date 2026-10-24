#pragma once
#include "reading/sleep_screen.h"

namespace kobalt::storage {
// One disposable, atomically replaced PNG with bounded metadata. The token
// identifies Nickel's last-read record, not an application release.
bool writeSleepSnapshot(const QString &path, const sleep::Snapshot &, const QString &token);
sleep::Snapshot readSleepSnapshot(const QString &path, const QString &token, sleep::Mode mode);
}
