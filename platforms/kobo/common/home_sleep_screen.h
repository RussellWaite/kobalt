#pragma once
#include "storage/reader_storage.h"
#include "reading/sleep_screen.h"
#include "platform/api/window_presentation.h"
#include <QObject>

namespace kobalt::nickel {
class HomeSleepScreen final : public QObject {
public:
    explicit HomeSleepScreen(storage::Paths paths, QObject *parent);
    void remember(const sleep::Snapshot &, const QString &identity);
    void forget();
    void prepare(bool enabled, bool powerOff = false);
    void preparePowerOff(sleep::Snapshot);
    void persistPowerOff(const QString &identity);
    void present();
    void clear();

private:
    bool eventFilter(QObject *, QEvent *) override;
    void updateBattery();
    storage::Paths paths;
    sleep::Snapshot snapshot, shutdownBackdrop;
    void save(const sleep::Snapshot &, const QString &identity);
    bool poweredOff = false, invalidated = false;
    void compose();
    std::unique_ptr<WindowPresentation> presentation;
};
}
