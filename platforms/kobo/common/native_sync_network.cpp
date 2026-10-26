#include "native_sync_network.h"
#include <QAbstractSocket>
#include <QHostAddress>
#include <QNetworkInterface>
#include <dlfcn.h>
#include <utility>

namespace kobalt {
namespace {
template <class T> T symbol(const char *name) { return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name)); }
NativeSyncNetworkBindings nativeBindings() {
    const auto instance = symbol<QObject *(*)()>("_ZN23WirelessWorkflowManager14sharedInstanceEv");
    const auto wirelessInstance = symbol<QObject *(*)()>("_ZN15WirelessManager14sharedInstanceEv");
    const auto up = symbol<bool (*)(QObject *)>("_ZNK15WirelessManager2upEv");
    const auto disabled = symbol<bool (*)(QObject *)>("_ZN23WirelessWorkflowManager14isAirplaneModeEv");
    const auto connecting = symbol<bool (*)(QObject *)>("_ZNK23WirelessWorkflowManager12isConnectingEv");
    const auto accessible = symbol<bool (*)(QObject *)>("_ZN23WirelessWorkflowManager20isInternetAccessibleEv");
    const auto request = symbol<void (*)(QObject *, bool, bool)>("_ZN23WirelessWorkflowManager15connectWirelessEbb");
    const auto silent = symbol<void (*)(QObject *)>("_ZN23WirelessWorkflowManager23connectWirelessSilentlyEv");
    const auto cancel = symbol<void (*)(QObject *)>("_ZN23WirelessWorkflowManager6cancelEv");
    if (!instance || !wirelessInstance || !up || !disabled || !connecting || !accessible || !request || !silent || !cancel) return {};
    QPointer<QObject> workflow = instance();
    QPointer<QObject> wireless = wirelessInstance();
    if (!workflow || !wireless) return {};
    NativeSyncNetworkBindings result;
    result.workflow = workflow;
    result.disabled = [workflow, disabled] { return !workflow || disabled(workflow); };
    result.connecting = [workflow, connecting] { return workflow && connecting(workflow); };
    result.ready = [workflow, wireless, up, accessible] {
        // up() is Nickel's cached radio state on both firmware generations.
        // An old DHCP address alone must not suppress a needed reconnect.
        if (!workflow || !wireless || !up(wireless)) return false;
        if (accessible(workflow)) return true;
        // A custom server can be reachable on the LAN even if Nickel's public
        // internet check fails. The HTTPS exchange decides server reachability.
        for (const auto &interface : QNetworkInterface::allInterfaces()) {
            const auto flags = interface.flags();
            if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
                (flags & QNetworkInterface::IsLoopBack)) continue;
            for (const auto &entry : interface.addressEntries())
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol &&
                    !entry.ip().isNull() && !entry.ip().isLoopback() &&
                    (entry.ip().toIPv4Address() & 0xffff0000u) != 0xa9fe0000u)
                    return true;
        }
        return false;
    };
    result.request = [workflow, request, silent](bool interactive) {
        if (!workflow) return;
        if (interactive) request(workflow, true, false);
        else silent(workflow);
    };
    result.cancel = [workflow, cancel] { if (workflow) cancel(workflow); };
    return result;
}
}
NativeSyncNetwork::NativeSyncNetwork() : NativeSyncNetwork(nativeBindings()) {}
NativeSyncNetwork::NativeSyncNetwork(NativeSyncNetworkBindings api)
    : bindings(std::move(api)), workflow(bindings.workflow) {
    elapsed.invalidate();
    poll.setInterval(100);
    connect(&poll, &QTimer::timeout, this, &NativeSyncNetwork::check);
    if (!bindings.valid()) return;
    // These InternetProvider signals are inherited by the wireless workflow on
    // both firmware generations. Polling also permits LAN-only connectivity.
    connect(workflow, SIGNAL(internetIsAccessible()), this, SLOT(check()));
    connect(workflow, SIGNAL(internetPartiallyAccessible()), this, SLOT(check()));
    connect(workflow, SIGNAL(connectingFailed()), this, SLOT(failed()));
    connect(workflow, SIGNAL(connectionCancelled()), this, SLOT(cancelled()));
    connect(workflow, &QObject::destroyed, this, [this] { finish(false); });
}
NativeSyncNetwork::~NativeSyncNetwork() { cancel(); }
bool NativeSyncNetwork::connected() const { return workflow && bindings.valid() && bindings.ready(); }
void NativeSyncNetwork::prepare(bool interactive, std::function<void(bool)> done) {
    cancel();
    if (connected()) { done(true); return; }
    if (!workflow || !bindings.valid() || (!interactive && bindings.disabled())) { done(false); return; }
    finished = std::move(done);
    elapsed.start();
    poll.start();
    if (!bindings.connecting()) {
        requested = true;
        bindings.request(interactive);
    }
}
void NativeSyncNetwork::check() {
    if (!finished) return;
    if (connected()) finish(true);
    else if (elapsed.elapsed() >= 10000) {
        auto done = std::move(finished);
        cancel();
        done(false);
    }
}
void NativeSyncNetwork::failed() { finish(connected()); }
void NativeSyncNetwork::cancelled() { finish(false); }
void NativeSyncNetwork::finish(bool ready) {
    poll.stop();
    requested = false;
    auto done = std::move(finished);
    finished = {};
    if (done) done(ready);
}
void NativeSyncNetwork::cancel() {
    poll.stop();
    finished = {};
    const bool owned = std::exchange(requested, false);
    if (owned && workflow && bindings.valid() && bindings.connecting()) bindings.cancel();
}
}
