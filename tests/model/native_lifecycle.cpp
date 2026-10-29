#include "platforms/kobo/common/native_lifecycle.h"
#include "storage/annotation_database.h"
#include "storage/profile_database.h"
#include "storage/reader_storage.h"
#include "storage/record_writer.h"
#include "check.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QSqlQuery>
#include <QVector>
#include <atomic>

// Nickel's exported managers and signals, resolved by the production adapter.
class N3PowerWorkflowManager : public QObject {
    Q_OBJECT
public:
    static N3PowerWorkflowManager *sharedInstance();
    bool sleeping() const;
    bool sleepViewVisible() const;
    bool pinCodeInputDialogVisible() const;
signals:
    void aboutToSleep();
    void resumingFromSleep();
    void resumedFromSleep();
};
N3PowerWorkflowManager *N3PowerWorkflowManager::sharedInstance() {
    static N3PowerWorkflowManager manager;
    return &manager;
}
bool N3PowerWorkflowManager::sleeping() const { return false; }
bool N3PowerWorkflowManager::sleepViewVisible() const { return false; }
bool N3PowerWorkflowManager::pinCodeInputDialogVisible() const { return false; }

class PlugWorkflowManager : public QObject {
    Q_OBJECT
public:
    static PlugWorkflowManager *sharedInstance();
    bool pluggedInViewVisible() const;
signals:
    void aboutToConnect();
    void aboutToUnplug();
    void doneProcessing();
};
PlugWorkflowManager *PlugWorkflowManager::sharedInstance() {
    static PlugWorkflowManager manager;
    return &manager;
}
bool PlugWorkflowManager::pluggedInViewVisible() const { return false; }

namespace {
QString profile;
int handoffs = 0, workflows = 0, shutdowns = 0;
bool available() {
    return kobalt::AnnotationDatabase::transaction(profile, [](QSqlDatabase &) { return true; });
}
void unused(void *) {}
void handoff(void *) {
    require(!available(), "USB handoff left the profile writable");
    require(!kobalt::NativeLifecycle::nativeSessionSaveAllowed(), "USB handoff allowed a late native session write");
    ++handoffs;
}
void workflow(void *manager) {
    require(!available(), "USB workflow resumed before the profile was blocked");
    ++workflows;
    kobalt::NativeLifecycle::interceptUsb(static_cast<QObject *>(manager));
}
void poweredOff(void *, bool) { ++shutdowns; }
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir root;
    profile = root.path();
    require(available(), "Cannot initialize lifecycle profile");
    kobalt::nativeShowSleepView = kobalt::nativeHideSleepView = unused;
    kobalt::nativeCanSuspend = [](void *) { return true; };
    kobalt::nativeUsbConnect = handoff;
    kobalt::nativeUsbWorkflow = workflow;
    kobalt::nativeAboutUsbConnect = kobalt::nativeReboot = unused;
    kobalt::nativePowerOff = poweredOff;
    auto *usb = PlugWorkflowManager::sharedInstance();
    N3PowerWorkflowManager::sharedInstance();
    kobalt::NativeLifecycle lifecycle(nullptr);
    require(lifecycle.connected(), "Mock Nickel managers did not connect");
    QVector<std::function<void()>> drains;
    lifecycle.prepareUsb = [&](std::function<void()> ready) { drains.append(std::move(ready)); };
    const auto remount = [&] {
        kobalt::NativeLifecycle::disconnected();
        require(!available(), "Disconnect reopened exported storage");
        emit usb->doneProcessing();
        require(!available(), "An ordinary scan reopened exported storage");
        emit usb->aboutToUnplug();
        emit usb->doneProcessing();
        require(available() && !lifecycle.blocked(), "Remount did not reopen the profile");
        require(kobalt::NativeLifecycle::nativeSessionSaveAllowed(), "Remount left native session saves blocked");
    };
    // Both native USB entry points must release the connection before handoff.
    for (bool fullWorkflow : {false, true}) {
        if (fullWorkflow)
            kobalt::NativeLifecycle::interceptUsbWorkflow(usb);
        else
            kobalt::NativeLifecycle::interceptUsb(usb);
        require(available(), "Pending USB preparation blocked storage before its final save");
        require(kobalt::NativeLifecycle::nativeSessionSaveAllowed(), "Pending USB blocked its native final save");
        const auto finish = drains.takeLast();
        finish();
        require(!available(), "USB preparation did not keep the profile closed");
        const int count = handoffs;
        finish();
        require(handoffs == count, "A duplicate completion repeated USB handoff");
        remount();
    }
    require(handoffs == 2 && workflows == 1, "USB entry points did not complete");
    // Cancel while the asynchronous reader drain is pending. Its late result
    // must not block storage again, including when a newer USB request exists.
    for (bool fullWorkflow : {false, true}) {
        const int before = handoffs;
        if (fullWorkflow)
            kobalt::NativeLifecycle::interceptUsbWorkflow(usb);
        else
            kobalt::NativeLifecycle::interceptUsb(usb);
        const auto cancelled = drains.takeLast();
        kobalt::NativeLifecycle::disconnected();
        cancelled();
        require(available() && !lifecycle.blocked() && handoffs == before,
                "Cancelled USB cleanup blocked the profile or exported storage");
        require(kobalt::NativeLifecycle::nativeSessionSaveAllowed(), "Cancelled USB blocked native session retry");
        kobalt::NativeLifecycle::interceptUsbWorkflow(usb);
        const auto current = drains.takeLast();
        cancelled();
        require(available() && lifecycle.storageBlocked(), "A stale completion affected the newer USB request");
        current();
        remount();
    }
    // A failed queued write still drains and permits USB handoff. Nickel must
    // not wait forever because a save failed on a full or failing volume.
    require(kobalt::AnnotationDatabase::transaction(profile, [](QSqlDatabase &db) {
        QSqlQuery query(db);
        return query.exec("CREATE TRIGGER refuse_save BEFORE INSERT ON book_state BEGIN SELECT RAISE(ABORT,'test'); END");
    }), "Cannot obstruct the final save");
    std::atomic_int saved{-1};
    kobalt::storage::RecordWriter writer;
    writer.post([] {
        QJsonObject state;
        state["book"] = "failed-save";
        return kobalt::storage::writeBookState(kobalt::storage::Paths{profile}, "failed-save", state);
    }, [&](bool ok) { saved = ok ? 1 : 0; });
    const int beforeFailure = handoffs;
    kobalt::NativeLifecycle::interceptUsb(usb);
    drains.takeLast()();
    require(saved == 0 && handoffs == beforeFailure + 1, "A failed save prevented USB handoff");
    remount();
    // Power-off supersedes a pending USB drain. A cable disconnect cannot
    // release the shutdown's storage barrier.
    kobalt::NativeLifecycle::interceptUsb(usb);
    const auto abandoned = drains.takeLast();
    lifecycle.prepareShutdown = [](std::function<void()> ready) {
        kobalt::storage::closeProfile(true);
        ready();
    };
    require(kobalt::NativeLifecycle::interceptShutdown(usb, kobalt::NativeLifecycle::Shutdown::PowerOff),
            "Power-off was not intercepted");
    kobalt::NativeLifecycle::disconnected();
    abandoned();
    require(shutdowns == 1 && !available(), "USB cancellation reopened storage during shutdown");
    require(!kobalt::NativeLifecycle::nativeSessionSaveAllowed(), "USB cancellation reopened native saves during shutdown");
    kobalt::storage::reopenProfile();
    kobalt::storage::closeProfile();
}

#include "native_lifecycle.moc"
