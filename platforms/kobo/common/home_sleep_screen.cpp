#include "home_sleep_screen.h"
#include "last_read_book.h"
#include "native_ui.h"
#include "window_presentation.h"
#include "interfaces/reader/views/sleep_screen.h"
#include "storage/sleep_snapshot.h"
#include "support/diagnostics.h"
#include <QFile>
#include <QApplication>
#include <QEvent>
#include <QSqlDatabase>
#include <dlfcn.h>
#include <future>

namespace kobalt::nickel {
namespace {
QString currentToken(const QString &book = {}) {
    const auto device = reinterpret_cast<const void *(*)()>(dlsym(RTLD_DEFAULT, "_ZN6Device16getCurrentDeviceEv"));
    const auto name = reinterpret_cast<const QString &(*)(const void *)>(
        dlsym(RTLD_DEFAULT, "_ZNK6Device9getDbNameEv"));
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    const auto connection = reinterpret_cast<QSqlDatabase (*)(QString, bool)>(
        dlsym(RTLD_DEFAULT, "_ZN15DatabaseAdapter12dbConnectionE7QStringb"));
#else
    const auto connection = reinterpret_cast<QSqlDatabase (*)(const QString &, bool)>(
        dlsym(RTLD_DEFAULT, "_ZN15DatabaseAdapter12dbConnectionERK7QStringb"));
#endif
    const auto *current = device ? device() : nullptr;
    if (!current || !name || !connection)
        return {};
    const auto native = connection(name(current), false);
    if (!native.isValid() || !native.isOpen())
        return {};
    QString token;
    const auto connectionName = QStringLiteral("kobalt-home-sleep");
    {
        auto database = QSqlDatabase::addDatabase("QSQLITE", connectionName);
        database.setDatabaseName(native.databaseName());
        database.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=100");
        if (database.open())
            token = lastReadBookToken(database, book);
        database.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
    return token;
}
}
HomeSleepScreen::HomeSleepScreen(storage::Paths paths, QObject *parent)
    : QObject(parent), paths(std::move(paths)), presentation(makeWindowPresentation(nullptr, nativeReaderSleepScreenAvailable())) {
    qApp->installEventFilter(this);
    observeNativeBattery(this, [this] { updateBattery(); });
}
void HomeSleepScreen::remember(const sleep::Snapshot &next, const QString &identity) {
    clear();
    save(next, identity);
}
void HomeSleepScreen::save(const sleep::Snapshot &next, const QString &identity) {
    // A failed write must leave the stock screen, rather than a stale page
    // from an earlier reading session.
    invalidated = true;
    QFile::remove(paths.sleepScreen());
    if (next.image.isNull() || next.mode == sleep::Mode::Stock)
        return;
    const auto token = currentToken(identity);
    if (token.isEmpty())
        return;
    // Encoding a screen-sized PNG takes most of a second on a Kobo, and the
    // reader saves it while it closes. Write it on another thread, so Home can
    // paint at once. clear() waits for it before the file is used or removed.
    pendingSave = std::async(std::launch::async, [path = paths.sleepScreen(), next, token] {
        return storage::writeSleepSnapshot(path, next, token);
    });
}
void HomeSleepScreen::finishSave() {
    if (!pendingSave.valid())
        return;
    if (pendingSave.get())
        invalidated = false;
    else
        diagnostic("sleep-snapshot-save-failed");
}
void HomeSleepScreen::forget() {
    clear();
    invalidated = true;
    QFile::remove(paths.sleepScreen());
}
void HomeSleepScreen::prepare(bool enabled, bool powerOff) {
    clear();
    if (!enabled || invalidated || !presentation->supportsSleepScreen())
        return;
    const auto preferences = storage::readPreferences(paths.preferences());
    if (preferences.status != storage::ReadStatus::Ok)
        return;
    snapshot = storage::readSleepSnapshot(paths.sleepScreen(), currentToken(), sleep::mode(preferences.object));
    snapshot.tile = sleep::tile(preferences.object);
    poweredOff = powerOff;
    compose();
}
void HomeSleepScreen::preparePowerOff(sleep::Snapshot next) {
    clear();
    shutdownBackdrop = next;
    snapshot = std::move(next);
    poweredOff = true;
    compose();
}
void HomeSleepScreen::persistPowerOff(const QString &identity) {
    if (poweredOff && !shutdownBackdrop.image.isNull()) {
        save(shutdownBackdrop, identity);
        // Nickel powers off once the shutdown continues.
        finishSave();
    }
    shutdownBackdrop = {};
}
bool HomeSleepScreen::eventFilter(QObject *object, QEvent *event) {
    if (poweredOff && event->type() == QEvent::Show && object->inherits("AbstractPowerView"))
        present();
    return false;
}
void HomeSleepScreen::compose() {
    if (snapshot.image.isNull())
        return;
    const auto battery = nativeReaderBattery();
    const auto status = poweredOff ? reader::SleepStatus::PoweredOff
                        : battery.fullyCharged ? reader::SleepStatus::Charged
                        : battery.charging ? reader::SleepStatus::Charging : reader::SleepStatus::Sleeping;
    snapshot.image = reader::sleepScreenImage(std::move(snapshot.image), snapshot.progress, snapshot.remaining,
                                             snapshot.dark, battery.percent, status, snapshot.dimPage, snapshot.tile,
                                             snapshot.percent);
    snapshot.dimPage = false;
}
void HomeSleepScreen::present() {
    if (!snapshot.image.isNull())
        presentation->presentSleepScreen(snapshot.image, snapshot.orientation);
}
void HomeSleepScreen::clear() {
    // Sleep, wake, USB and power-off all clear first. None of them may meet a
    // half-written file or unmount storage under the writer.
    finishSave();
    presentation->clearSleepScreen();
    snapshot = {};
    shutdownBackdrop = {};
    poweredOff = false;
}
void HomeSleepScreen::updateBattery() {
    if (snapshot.image.isNull() || poweredOff)
        return;
    const auto battery = nativeReaderBattery();
    const auto status = battery.fullyCharged ? reader::SleepStatus::Charged
                        : battery.charging ? reader::SleepStatus::Charging : reader::SleepStatus::Sleeping;
    snapshot.image = reader::sleepScreenImage(std::move(snapshot.image), snapshot.progress, snapshot.remaining,
                                             snapshot.dark, battery.percent, status, false, snapshot.tile,
                                             snapshot.percent);
    presentation->updateSleepScreen(
        snapshot.image, reader::sleepScreenCardRect(snapshot.image.size(), !snapshot.remaining.isEmpty(), snapshot.tile));
}
}
