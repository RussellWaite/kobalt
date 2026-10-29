#pragma once

// Included only by evaluation builds. Overrides never enter saved preferences.
namespace kobalt::scenario {
struct SleepPersistenceOptions {
    int mode = -1;
    int compression = 1;
};
inline SleepPersistenceOptions &sleepPersistenceOptions() {
    static SleepPersistenceOptions options;
    return options;
}
}
