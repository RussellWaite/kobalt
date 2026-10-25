#pragma once
#include <QWidget>
#include "reading/sleep_screen.h"
#include <functional>
#include <memory>

namespace kobalt {
class ReaderSession;
class ReaderOrientation;
class ReadingBook;
struct ReaderHostServices;

// The firmware controller owns opening, authorization and publication lifetime.
// The window returns a retained engine session only after a normal close.
class ReaderWindow : public QWidget {
public:
    using QWidget::QWidget;
    std::function<void(std::unique_ptr<ReaderSession>)> finished;
    virtual bool readyForPresentation() const = 0;
    // Prepare optional page-bound content before the host replaces its opening view.
    // Empty preparation completes synchronously. Cancellation destroys the window.
    virtual void preparePresentation(std::function<void(bool)> finished) {
        finished(true);
    }
    virtual void checkpoint() = 0;
    // Asked before the device enters standby. False means reading-position
    // saves are pending; the window starts them and the host should ask again.
    virtual bool flushBeforeStandby() {
        return true;
    }
    // Limit the native-position wait to two seconds before file release.
    virtual void prepareUsb(std::function<void()> finished) = 0;
    virtual void prepareShutdown(std::function<void()> finished) = 0;
    virtual sleep::Snapshot powerScreenSnapshot() { return {}; }
    virtual void sleepReader() = 0;
    // A bounded final upload can briefly keep the CPU awake after the sleep view appears.
    virtual bool pendingNetworkSync() const { return false; }
    virtual void prepareWake() = 0;
    virtual void cancelWake() = 0;
    virtual void sleepViewReady() = 0;
    virtual void wakeReader() = 0;
    virtual void progressTick() = 0;
    virtual void refreshHostPresentation() = 0;
    // Logical forward/backward input, independent of touch swipe direction.
    virtual void pageButton(int direction) = 0;
};

ReaderWindow *createReaderWindow(std::unique_ptr<ReadingBook> book, ReaderOrientation *orientation,
                                 ReaderHostServices services);
}
