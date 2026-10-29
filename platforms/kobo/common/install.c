#include <stddef.h>
#include <resources.h>
#include "embedded_resources.h"
#include "startup_recovery.h"

// NickelHook calls this before loading Nickel libraries or installing hooks.
bool nh_startup_failed(void) {
    return kobalt_startup_remove(KOBALT_CONFIG_DIR "/startup-session");
}

const struct nh_resources nh_resources = {
    .config_dir = KOBALT_CONFIG_DIR,
    .uninstall_file = KOBALT_CONFIG_DIR "/uninstall-now",
    .files = nh_embedded_resources,
    .initialized_file = KOBALT_CONFIG_DIR "/.initialized",
};
