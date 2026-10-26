#include "platforms/kobo/common/native_lifecycle.h"
#include "storage/annotation_database.h"
#include "storage/profile_database.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QTest>

// Export the native entry points and signals used by the real lifecycle adapter.
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
signals:
    void doneProcessing();
    void aboutToUnplug();
};
PlugWorkflowManager *PlugWorkflowManager::sharedInstance() {
    static PlugWorkflowManager manager;
    return &manager;
}

namespace {
int powerOffs = 0, reboots = 0;
bool batteryLow = false;
QString profilePath;
bool profileClosedAtHandoff = false;
void ignore(void *) {}
bool canSuspend(void *) { return true; }
void recordHandoff() {
    profileClosedAtHandoff = !kobalt::AnnotationDatabase::transaction(profilePath, [](QSqlDatabase &) { return true; });
}
void powerOff(void *, bool low) { recordHandoff(); ++powerOffs; batteryLow = low; }
void reboot(void *) { recordHandoff(); ++reboots; }
}

class ShutdownTests : public QObject {
    Q_OBJECT
private slots:
    void init() {
        powerOffs = reboots = 0;
        batteryLow = false;
        profileClosedAtHandoff = false;
        kobalt::nativeShowSleepView = kobalt::nativeHideSleepView = ignore;
        kobalt::nativeUsbConnect = kobalt::nativeUsbWorkflow = kobalt::nativeAboutUsbConnect = ignore;
        kobalt::nativeCanSuspend = canSuspend;
        kobalt::nativePowerOff = powerOff;
        kobalt::nativeReboot = reboot;
        kobalt::storage::reopenProfile();
    }
    void cleanup() {
        kobalt::storage::reopenProfile();
        kobalt::storage::closeProfile();
    }
    void completion_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("timeout");
        for (int mode = 0; mode < 3; ++mode) {
            const auto name = QByteArray::number(mode);
            QTest::newRow((name + "-drained").constData()) << mode << false;
            QTest::newRow((name + "-stalled").constData()) << mode << true;
        }
    }
    void completion() {
        QFETCH(int, mode);
        QFETCH(bool, timeout);
        QTemporaryDir profile;
        profilePath = profile.path();
        const auto available = [&] {
            return kobalt::AnnotationDatabase::transaction(profile.path(), [](QSqlDatabase &) { return true; });
        };
        QVERIFY(available());
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        std::function<void()> drained;
        lifecycle.prepareShutdown = [&](std::function<void()> ready) { drained = std::move(ready); };
        QObject manager;
        QVERIFY(kobalt::NativeLifecycle::interceptShutdown(&manager, kobalt::NativeLifecycle::Shutdown(mode)));
        QVERIFY(drained);
        QCOMPARE(powerOffs + reboots, 0);
        if (timeout)
            QTRY_COMPARE_WITH_TIMEOUT(powerOffs + reboots, 1, 5000);
        else
            drained();
        QCOMPARE(reboots, mode == 2 ? 1 : 0);
        QCOMPARE(powerOffs, mode == 2 ? 0 : 1);
        QCOMPARE(batteryLow, mode == 1);
        QVERIFY2(profileClosedAtHandoff, "The native action ran before the profile was blocked");
        QVERIFY2(!available(), "Power-off left the shared profile open or writable");
        kobalt::NativeLifecycle::disconnected();
        QVERIFY2(!available(), "USB disconnect reopened the profile during power-off");
        drained();
        drained();
        QVERIFY(kobalt::NativeLifecycle::interceptShutdown(&manager, kobalt::NativeLifecycle::Shutdown(mode)));
        QCOMPARE(powerOffs + reboots, 1);
    }
    void destroyedLifecycleIgnoresCompletion() {
        std::function<void()> drained;
        QObject manager;
        {
            kobalt::NativeLifecycle lifecycle(nullptr);
            lifecycle.prepareShutdown = [&](std::function<void()> ready) { drained = std::move(ready); };
            QVERIFY(kobalt::NativeLifecycle::interceptShutdown(&manager, kobalt::NativeLifecycle::Shutdown::PowerOff));
        }
        drained();
        QCOMPARE(powerOffs + reboots, 0);
    }
    void destroyedManagerIgnoresCompletion() {
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        std::function<void()> drained;
        lifecycle.prepareShutdown = [&](std::function<void()> ready) { drained = std::move(ready); };
        auto *manager = new QObject;
        QVERIFY(kobalt::NativeLifecycle::interceptShutdown(manager, kobalt::NativeLifecycle::Shutdown::PowerOff));
        delete manager;
        drained();
        QCOMPARE(powerOffs + reboots, 0);
    }
};
QTEST_GUILESS_MAIN(ShutdownTests)
#include "kobo_shutdown.moc"
