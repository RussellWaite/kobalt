#include "platforms/kobo/common/native_sync_network.h"
#include "check.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

class Workflow final : public QObject {
    Q_OBJECT
signals:
    void internetIsAccessible();
    void internetPartiallyAccessible();
    void connectingFailed();
    void connectionCancelled();
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    Workflow workflow;
    bool ready = true, disabled = false, connecting = false, interactive = false;
    int requests = 0, cancelled = 0, replies = 0;
    bool result = false;
    kobalt::NativeSyncNetworkBindings bindings;
    bindings.workflow = &workflow;
    bindings.ready = [&] { return ready; };
    bindings.disabled = [&] { return disabled; };
    bindings.connecting = [&] { return connecting; };
    bindings.request = [&](bool showUi) { ++requests; interactive = showUi; connecting = true; };
    bindings.cancel = [&] { ++cancelled; connecting = false; emit workflow.connectionCancelled(); };
    auto done = [&](bool connected) { ++replies; result = connected; };
    kobalt::NativeSyncNetwork network(bindings);
    network.prepare(false, done);
    require(replies == 1 && result && requests == 0, "reuse an available connection without touching the radio");
    ready = false;
    disabled = true;
    network.prepare(false, done);
    require(replies == 2 && !result && requests == 0, "automatic reconnect respects explicitly disabled Wi-Fi");
    network.prepare(true, done);
    require(requests == 1 && interactive && replies == 2, "manual sync starts the native interactive workflow and waits");
    ready = true;
    connecting = false;
    emit workflow.internetPartiallyAccessible();
    require(replies == 3 && result, "a usable connection can proceed when internet access is partial");
    emit workflow.internetIsAccessible();
    require(replies == 3, "duplicate readiness notifications do not start another exchange");
    ready = false;
    disabled = false;
    network.prepare(false, done);
    require(requests == 2 && !interactive, "automatic opening uses silent reconnect");
    connecting = false;
    emit workflow.connectingFailed();
    require(replies == 4 && !result, "native connection failure completes the wait once");
    network.prepare(true, done);
    network.cancel();
    require(cancelled == 1 && replies == 4, "cancel stops an owned native workflow without completing a stale wait");
    ready = true;
    emit workflow.internetIsAccessible();
    require(replies == 4, "late connection success after cancellation is ignored");
    network.prepare(false, done);
    require(replies == 5 && result, "a fresh attempt works after cancellation");
    ready = false;
    connecting = true;
    const int before = requests;
    network.prepare(true, done);
    require(requests == before, "join an existing workflow instead of starting another one");
    network.cancel();
    require(cancelled == 1, "cancelling this waiter does not cancel another native caller's workflow");
    connecting = false;
    {
        kobalt::NativeSyncNetwork pending(bindings);
        pending.prepare(false, done);
    }
    require(cancelled == 2 && replies == 5, "destruction releases an owned wait and suppresses its callback");
    kobalt::NativeSyncNetwork unavailable(kobalt::NativeSyncNetworkBindings{});
    unavailable.prepare(true, done);
    require(replies == 6 && !result, "unsupported native APIs fail without a connection attempt");
    QEventLoop deadline;
    network.prepare(false, [&](bool connected) { done(connected); deadline.quit(); });
    QTimer::singleShot(12000, &deadline, &QEventLoop::quit);
    deadline.exec();
    require(replies == 7 && !result && cancelled == 3, "a stalled native workflow is cancelled at the bounded deadline");
    require(!connecting, "the deadline leaves no owned connection workflow running");
    network.prepare(false, done);
    ready = true;
    connecting = false;
    emit workflow.connectingFailed();
    require(replies == 8 && result, "a failed native internet probe does not reject an available LAN connection");
    ready = false;
    network.prepare(true, done);
    ready = true;
    connecting = false;
    emit workflow.connectionCancelled();
    require(replies == 9 && !result, "explicit native cancellation wins even if an address became available");
}
#include "native_sync_network.moc"
