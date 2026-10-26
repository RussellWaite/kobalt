#include "native_sync_network.h"
#include "native_network_state.h"
#include "native_dialog_overlay.h"
#include "support/diagnostics.h"
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
    result.inputPending = [] { return nickel::NativeDialogOverlay::inputPending(); };
    result.disabled = [workflow, disabled] { return !workflow || disabled(workflow); };
    result.connecting = [workflow, connecting] { return workflow && connecting(workflow); };
    result.ready = [workflow, wireless, up, accessible] {
        // up() is Nickel's cached radio state on both firmware generations.
        // An old DHCP address alone must not suppress a needed reconnect.
        if (!workflow || !wireless || !up(wireless)) return false;
        if (accessible(workflow)) return true;
        // A custom server can be reachable on the LAN even if Nickel's public
        // internet check fails. The HTTPS exchange decides server reachability.
        return nickel::usableLocalNetwork();
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
void NativeSyncNetwork::prepare(bool showUi, std::function<void(bool)> done) {
    cancel();
    const bool available = connected();
    diagnostic("kosync-network-prepare", qt::object({{"interactive", showUi}, {"ready", available},
        {"bindings_available", bool(workflow) && bindings.valid()}}));
    if (available) { done(true); return; }
    if (!workflow || !bindings.valid() || (!showUi && bindings.disabled())) { done(false); return; }
    interactive = showUi;
    finished = std::move(done);
    elapsed.start();
    poll.setInterval(100);
    poll.start();
    if (!bindings.connecting()) {
        requested = true;
        bindings.request(showUi);
    }
}
void NativeSyncNetwork::check() {
    if (!finished) return;
    if (interactive && bindings.inputPending && bindings.inputPending()) {
        elapsed.restart();
        poll.setInterval(1000);
        return;
    }
    poll.setInterval(100);
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
    diagnostic("kosync-network-finished", qt::object({{"ready", ready}, {"owned", requested}}));
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
    if (owned && workflow && bindings.valid()) bindings.cancel();
}
}
